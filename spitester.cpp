//
// Copyright (C) Microsoft. All rights reserved.
//
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <lpc17xx.h>

#include "lldtester.h"
#include "util.h"
#include "Lpc17xxHardware.h"
#include "spitester.h"

using namespace Lldt::Spi;

uint32_t SpiTester::dummy;
volatile uint32_t SpiTester::remainingInterrupts;

namespace { // static

//
// Configure P0.6 as MAT2.0
//
void MuxInterruptOutput ()
{
    // P0.6 - MAT2.0 - O - Match output for Timer 2, channel 0.
    LPC_PINCON->PINSEL0 |= 0x3 << 12;
}

//
// Configure P0.6 as GPIO input
//
void DemuxInterruptOutput ()
{
    // P0.6 - I/O - General purpose digital input/output pin.
    LPC_PINCON->PINSEL0 &= ~(0x3 << 12);
}

//
// Enabling falling edge detection for P0.15 (SCK0)
//
void EnableSckFallingEdgeDetection ()
{
    LPC_GPIOINT->IO0IntEnR &= ~(1 << 15);
    LPC_GPIOINT->IO0IntClr = 1 << 15;
    LPC_GPIOINT->IO0IntEnF |= 1 << 15;
}

//
// Disable interrupt flag on SCK falling edge
//
void DisableSckFallingEdgeDetection ()
{
    LPC_GPIOINT->IO0IntEnF &= ~(1 << 15);
}

//
// Waits for the next falling edge of SCK.
//
void WaitForSckFallingEdge ()
{
    LPC_GPIOINT->IO0IntClr = 1 << 15;
    while (!(LPC_GPIOINT->IO0IntStatF & (1 << 15)));
}


} // namespace "static"

void SpiTester::Init ()
{
    SspInit();
    TimerInit();

    uint32_t sspClk = GetPeripheralClockFrequency(CLKPWR_PCLKSEL_SSP0);

    this->testerInfo.DeviceId = DEVICE_ID;
    this->testerInfo.Version = VERSION;
    this->testerInfo.MaxFrequency = std::min(uint32_t(5000000), sspClk / 12);
    this->testerInfo.ClockMeasurementFrequency = SystemCoreClock;
    this->testerInfo.MinDataBitLength = MIN_DATA_BIT_LENGTH;
    this->testerInfo.MaxDataBitLength = MAX_DATA_BIT_LENGTH;

    this->transferInfo = TransferInfo();
    this->interruptInfo = PeriodicInterruptInfo();

    DBGPRINT(
        "sspClk = %lu, Maximum clock rate = %lu\n\r",
        sspClk,
        this->testerInfo.MaxFrequency);
}

//
// Initialize SSP0 in slave mode
//
void SpiTester::SspInit ()
{
    // Power
    SetPeripheralPowerState(CLKPWR_PCONP_PCSSP0, true);

    // Clock (set to maximum)
    SetPeripheralClockDivider(CLKPWR_PCLKSEL_SSP0, CLKPWR_PCLKSEL_CCLK_DIV_1);

    // Configure Pins

    // SCK0 (P0.15)
    LPC_PINCON->PINSEL0 = (LPC_PINCON->PINSEL0 & ~(0x3 << 30)) | (0x2 << 30);

    // SSEL0 (P0.16), MISO0 (P0.17), MOSI (P0.18)
    uint32_t temp = 
        (LPC_PINCON->PINSEL1 & ~((0x3 << 2) | (0x3 << 4) | (0x3 << 0)));
    temp |= (0x2 << 2) | (0x2 << 4) | (0x2 << 0);
    LPC_PINCON->PINSEL1 = temp;

    // Disable interrupts
    LPC_SSP0->IMSC = 0;
    LPC_SSP0->CPSR = 2;

    // Program control registers and enable
    SspSetDataMode(
        SPI_CONTROL_INTERFACE_MODE,
        SPI_CONTROL_INTERFACE_DATABITLENGTH);
}

void SpiTester::SspSetDataMode (SpiDataMode Mode, uint32_t DataBitLength)
{
    uint32_t cr0 = SSP_CR0_FRF_SPI;

    switch (Mode) {
    case SpiDataMode::Mode1:
    case SpiDataMode::Mode2:
        cr0 |= SSP_CR0_CPHA_SECOND;
        break;
    case SpiDataMode::Mode0:
    case SpiDataMode::Mode3:
    default:
        cr0 |= SSP_CR0_CPOL_HI | SSP_CR0_CPHA_SECOND;
        break;
    }

    if ((DataBitLength >= MIN_DATA_BIT_LENGTH) &&
        (DataBitLength <= MAX_DATA_BIT_LENGTH)) {

        cr0 |= SSP_CR0_DSS(DataBitLength);
    } else {
        cr0 |= SSP_CR0_DSS(8);
    }

    LPC_SSP0->CR1 = SSP_CR1_SLAVE_EN;
    LPC_SSP0->CR0 = cr0;
    LPC_SSP0->CR1 = SSP_CR1_SSP_EN | SSP_CR1_SLAVE_EN;
}

//
// Data.Header.Length must be already set to the total length of the structure
//
void SpiTester::SspSendImpl (TransferHeader& Data)
{
    const uint8_t* const beginBytePtr = reinterpret_cast<const uint8_t*>(&Data);

    // Compute checksum
    Data.Header.Checksum = 0;
    Data.Header.Checksum = Crc16().Update(beginBytePtr, Data.Header.Length);

    // precondition: FIFO must be empty
    if (!(LPC_SSP0->SR & SSP_SR_TFE)) {
        DBGPRINT("SSP transmit fifo is not empty!\n\r");
        return;
    }

    // preload FIFO with data
    const uint8_t* bytePtr;
    {
        const uint8_t* const preloadEndPtr =
            beginBytePtr + std::min<uint32_t>(Data.Header.Length, 8);
        for (bytePtr = beginBytePtr; bytePtr != preloadEndPtr; ++bytePtr) {
            LPC_SSP0->DR = *bytePtr;
        }
    }

    // wait for the transfer to begin
    while (!ChipSelectAsserted());

    // disable interrupts and send data
    bool transmitUnderrun = false;
    {
        DisableIrq disableIrq;

        const uint8_t* const endBytePtr = beginBytePtr + Data.Header.Length;
        while (bytePtr != endBytePtr) {
            uint32_t status = LPC_SSP0->SR;

            if (status & SSP_SR_TFE) {
                // If transmit FIFO is empty, a transmit underrun occurred
                transmitUnderrun = true;
            }

            if (status & SSP_SR_TNF) {
                LPC_SSP0->DR = *bytePtr;
                ++bytePtr;
            }

            if (!ChipSelectAsserted()) {
                return;
            }
        }
    } // enable IRQ

    WaitForCsToDeassert();

    if (transmitUnderrun) {
        DBGPRINT("Transmit underrun occurred!\n\r");
    }
}

void SpiTester::WaitForCsToDeassert ()
{
    while (ChipSelectAsserted() || (LPC_SSP0->SR & SSP_SR_RNE))
        dummy = LPC_SSP0->DR;
}

//
// Initialize TIMER2 to capture inputs on CAP2.0
//
void SpiTester::TimerInit ()
{
    // Initialize clock and power, use highest posible resolution
    SetPeripheralPowerState(CLKPWR_PCONP_PCTIM2, true);
    SetPeripheralClockDivider(CLKPWR_PCLKSEL_TIMER2, CLKPWR_PCLKSEL_CCLK_DIV_1);

    // P0.4 - CAP2.0 - I - Capture input for Timer 2, channel 0.
    LPC_PINCON->PINSEL0 |= 0x3 << 8;

    // Put timer in reset
    LPC_TIM2->TCR = TIM_TCR_RESET;

    // Timer mode
    LPC_TIM2->TCR = 0;

    // Increment Timer Counter on every PCLK
    LPC_TIM2->PR = 0;

    // Ensure MAT2.0 is initially high
    LPC_TIM2->EMR |= (1U << TIM_MATCH_CHANNEL_0);
}

ClockMeasurementStatus SpiTester::WaitForCapture (uint32_t* CapturePtr)
{
    // wait for first capture or first byte to be received
    uint32_t capture;

    // Check the CR0 register more frequently than the RNE register so that
    // CR0 doesn't get overwritten by the next falling edge.
    while (!(LPC_SSP0->SR & SSP_SR_RNE)) {
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
        if ((capture = LPC_TIM2->CR0) != 0) break;
    }

    if (capture != 0) {
        *CapturePtr = capture;
        return ClockMeasurementStatus::Success;
    }

    // give approximation of first falling edge based on timer register
    *CapturePtr = LPC_TIM2->TC;
    return ClockMeasurementStatus::EdgeNotDetected;
}

TransferInfo SpiTester::CaptureTransfer (const CommandBlock& Command)
{
    auto transferInfo = TransferInfo();

    uint32_t checksum = 0;
    const uint32_t dataMask = 
        (1 << Command.u.CaptureNextTransfer.DataBitLength) - 1;
    // This is the value we should expect to receive from the master
    uint32_t rxValue = Command.u.CaptureNextTransfer.SendValue;
    // This is the value we should send to the master
    uint32_t txValue = Command.u.CaptureNextTransfer.ReceiveValue;
    bool mismatchDetected = false;

    SspSetDataMode(
        SpiDataMode(Command.u.CaptureNextTransfer.Mode),
        Command.u.CaptureNextTransfer.DataBitLength);

    // Put timer in reset
    LPC_TIM2->TCR = TIM_TCR_RESET;

    // Stop the counter if overflow is detected
    LPC_TIM2->MCR = TIM_MCR_STOP_ON_MATCH(TIM_MATCH_CHANNEL_0);
    LPC_TIM2->MR0 = 0xffffffff;

    // Capture CR0 on falling edge
    LPC_TIM2->CCR = TIM_CCR_FALLING(TIM_CAPTURE_CHANNEL_0);

    __disable_irq();

    // do initial fill of TX fifo
    for (int i = 0; i < 8; ++i) {
        LPC_SSP0->DR = txValue & dataMask;
        ++txValue;
    }

    // Wait for CS to assert
    while (!ChipSelectAsserted());

    // start timer
    LPC_TIM2->TCR = TIM_TCR_ENABLE;

    uint32_t capture1;
    transferInfo.ClockActiveTimeStatus = WaitForCapture(&capture1);

    for (;;) {
        // byte received?
        uint32_t status = LPC_SSP0->SR;

        if (status & SSP_SR_RNE) {
            uint32_t data = LPC_SSP0->DR;

            //add to checksum
            checksum = crc16_update(checksum, uint8_t(data));
            // checksum.Update(uint8_t(data));
            if (dataMask & (1 << 8)) {
                checksum = crc16_update(checksum, uint8_t(data >> 8));
            }

            if ((data != (rxValue & dataMask)) && !mismatchDetected) {
                mismatchDetected = true;
                transferInfo.MismatchIndex =
                    rxValue - Command.u.CaptureNextTransfer.SendValue;
            }
            ++rxValue;
        } else if (!ChipSelectAsserted()) {
            // only check if chip select is deasserted if the receive FIFO
            // has been purged
            break;
        }

        // space available in TX FIFO?
        if (status & SSP_SR_TNF) {
            LPC_SSP0->DR = txValue & dataMask;
            ++txValue;
        }
    }

    __enable_irq();

    if (transferInfo.ClockActiveTimeStatus == ClockMeasurementStatus::Success) {
        // did timer overflow?
        if (!(LPC_TIM2->TCR & TIM_TCR_ENABLE)) {
            transferInfo.ClockActiveTimeStatus = 
                ClockMeasurementStatus::Overflow;
        } else {
            // measurement was captured successfully
            uint32_t capture2 = LPC_TIM2->CR0;
            LPC_TIM2->TCR = TIM_TCR_RESET;

            transferInfo.ClockActiveTime = capture2 - capture1;
        }
    }

    transferInfo.Checksum = checksum;
    transferInfo.ElementCount = 
        rxValue - Command.u.CaptureNextTransfer.SendValue;
    if (!mismatchDetected)
        transferInfo.MismatchIndex = transferInfo.ElementCount;

    SspSetDataMode(
        SPI_CONTROL_INTERFACE_MODE,
        SPI_CONTROL_INTERFACE_DATABITLENGTH);

    return transferInfo;
}

extern "C" void TIMER2_IRQHandler ()
{
    LPC_TIM2->IR = TIM_IR_MASK;
    if (--SpiTester::remainingInterrupts == 0) {
        // Disable falling edge generation, but still need to keep clock
        // running for latency calculation of final interrupt
        LPC_TIM2->TCR = TIM_TCR_RESET;
        LPC_TIM2->MCR = 0;
        LPC_TIM2->TCR = TIM_TCR_ENABLE;
    }
}

PeriodicInterruptInfo SpiTester::RunPeriodicInterrupts (
    const CommandBlock& Command
    )
{
    DBGPRINT("Entering periodic interrupt mode\n\r");
    auto interruptInfo = PeriodicInterruptInfo();

    // program timer to bring external match output low, reset, and
    // generate interrupt
    const uint32_t period = this->testerInfo.ClockMeasurementFrequency /
        Command.u.StartPeriodicInterrupts.InterruptFrequency;
    uint32_t interruptCount;
    {
        // Put timer in reset
        LPC_TIM2->TCR = TIM_TCR_RESET;
        LPC_TIM2->IR = TIM_IR_MASK;

        // On period signal, generate interrupt and reset
        LPC_TIM2->MCR =
            TIM_MCR_INT_ON_MATCH(TIM_MATCH_CHANNEL_0) |
            TIM_MCR_RESET_ON_MATCH(TIM_MATCH_CHANNEL_0);

        LPC_TIM2->MR0 = period;

        // Bring channel 0 low (P4.28) on match, and ensure that match channel
        // is initially high
        LPC_TIM2->EMR = (1U << TIM_MATCH_CHANNEL_0) |
            TIM_EMR_LOW_ON_MATCH(TIM_MATCH_CHANNEL_0);

        LPC_TIM2->CCR = 0;

        if (!Command.u.StartPeriodicInterrupts.ComputeInterruptCount(
                interruptCount)) {

            DBGPRINT(
                "Interrupt count overflow. "
                "(DurationInSeconds=%d, InterruptFrequency=%lu)\n\r",
                Command.u.StartPeriodicInterrupts.DurationInSeconds,
                Command.u.StartPeriodicInterrupts.InterruptFrequency);

            interruptInfo.Status.s.ArithmeticOverflow = true;
            return interruptInfo;
        }
        this->remainingInterrupts = interruptCount;

        // Start generating falling edges on the external match pin
        MuxInterruptOutput();
        NVIC_EnableIRQ(TIMER2_IRQn);
        LPC_TIM2->TCR = TIM_TCR_ENABLE;
    }

    uint32_t alreadyAckedCount = 0;
    uint32_t ackedPastDeadlineCount = 0;
    uint32_t ackedBeforeDeadlineCount = 0;
    uint32_t lastAckedInterruptCount = interruptCount;

    // Enable falling edge detection for SCK0 (P0.15)
    EnableSckFallingEdgeDetection();

    auto finally = Finally([&] {
        DisableSckFallingEdgeDetection();

        // Put timer in reset to disable interrupts
        LPC_TIM2->TCR = TIM_TCR_RESET;
        NVIC_DisableIRQ(TIMER2_IRQn);

        // De-assert and demux the interrupt signal
        LPC_TIM2->EMR = (1U << TIM_MATCH_CHANNEL_0);
        DemuxInterruptOutput();
        ActLedOff();
    });

    while (this->remainingInterrupts) {
        // Clear receive FIFO and queue 8 dummy bytes to output FIFO
        static_assert(
            sizeof(CommandBlock) == 8,
            "Verifying that CommandBlock is the same size as the FIFO");

        for (int i = sizeof(CommandBlock); i; --i) {
            LPC_SSP0->DR = 0;
            this->dummy = LPC_SSP0->DR;
        }

        DBGPRINT(
            "Waiting for SCK to assert. (Rx Fifo empty = %ld)\n\r",
            LPC_SSP0->SR & SSP_SR_RNE);

        // wait for falling edge of SCK. While we're waiting, the timer match
        // will be reached, the interrupt signal will be asserted, and the
        // interrupt count will be incremented.
        WaitForSckFallingEdge();
        uint32_t capture = LPC_TIM2->TC;

        // deassert interrupt signal
        LPC_TIM2->EMR |= (1U << TIM_MATCH_CHANNEL_0);

        DisableIrq disableIrq;

        // capture and verify the first byte received. If it is not
        // AcknowledgeInterrupt, leave interrupt mode
        {
            while (!(LPC_SSP0->SR & SSP_SR_RNE)) {
                if (!ChipSelectAsserted()) {
                    interruptInfo.Status.s.IncompleteReceive = true;
                    return interruptInfo;
                }
            }

            uint32_t commandByte = LPC_SSP0->DR;
            if (commandByte != SpiTesterCommand::AcknowledgeInterrupt) {
                interruptInfo.Status.s.NotAcknowledged = true;
                WaitForCsToDeassert();
                return interruptInfo;
            }
        }

        // prepare the output buffer that we'll send back to the client
        // in response to the AcknowledgeInterrupt command
        AcknowledgeInterruptInfo ackInfo;
        {
            int difference =
                lastAckedInterruptCount - this->remainingInterrupts;

            if (difference < 0) {
                // this should never happen
                interruptInfo.Status.s.ArithmeticOverflow = true;
                return interruptInfo;
            } else if (difference == 0) {
                // this interrupt was already acknowledged
                // Use a bogus value for TiemSinceFallingEdge so it does not get
                // included in latency calculations
                ++alreadyAckedCount;
                ackInfo.TimeSinceFallingEdge = INVALID_TIME_SINCE_FALLING_EDGE;
            } else if (difference == 1) {
                // this interrupt was acknowledged before the deadline
                ++ackedBeforeDeadlineCount;
                ackInfo.TimeSinceFallingEdge = capture;
            } else {
                // not acknowledged by the deadline
                ++ackedPastDeadlineCount;
                ackInfo.TimeSinceFallingEdge =
                    (difference - 1) * period + capture;
            }
            lastAckedInterruptCount -= difference;

            // Use a very simple checksum so that we can meet the SPI transfer
            // deadline
            ackInfo.Checksum = ~ackInfo.TimeSinceFallingEdge;
        }

        // Send out the response
        const uint8_t* const endPtr =
            reinterpret_cast<const uint8_t*>(&ackInfo) + sizeof(ackInfo);
        for (const uint8_t* dataPtr = reinterpret_cast<const uint8_t*>(&ackInfo);
             dataPtr != endPtr;) {

            uint32_t status = LPC_SSP0->SR;

            if (status & SSP_SR_TFE) {
                interruptInfo.Status.s.TransmitUnderrun = true;
                break;
            }

            // space available in TX FIFO?
            if (status & SSP_SR_TNF) {
                LPC_SSP0->DR = *dataPtr;
                ++dataPtr;
            }

            if (!ChipSelectAsserted()) {
                interruptInfo.Status.s.IncompleteTransmit = true;
                break;
            }
        }

        WaitForCsToDeassert();
    }

    if (this->remainingInterrupts != 0) {
        DBGPRINT("remainingInterrupts is not zero!");
    }

    interruptInfo.InterruptCount = interruptCount;
    interruptInfo.AcknowledgedBeforeDeadlineCount = ackedBeforeDeadlineCount;
    interruptInfo.AcknowledgedAfterDeadlineCount = ackedPastDeadlineCount;
    interruptInfo.AlreadyAcknowledgedCount = alreadyAckedCount;

    DBGPRINT(
        "Leaving interrupt mode. "
        "(negativeCountalreadyAckedCount=%lu, ackedPastDeadlineCount=%lu, "
        "ackedBeforeDeadlineCount=%lu, interruptCount=%lu\n\r",
        alreadyAckedCount,
        ackedPastDeadlineCount,
        ackedBeforeDeadlineCount,
        interruptCount);

    return interruptInfo;
}

bool SpiTester::ReceiveCommand (CommandBlock& Command)
{
    // is there any data waiting for us?
    if (!(LPC_SSP0->SR & SSP_SR_RNE)) return false;

    // receive a command block
    for (size_t i = 0; i < sizeof(Command); ) {
        // byte received?
        if (LPC_SSP0->SR & SSP_SR_RNE) {
            uint32_t data = LPC_SSP0->DR;
            reinterpret_cast<uint8_t*>(&Command)[i] = uint8_t(data);
            ++i;
        } else if (!ChipSelectAsserted()) {
            return false;
        }
    }

    WaitForCsToDeassert();
    return true;
}

void SpiTester::RunStateMachine ()
{
    CommandBlock command;
    if (ReceiveCommand(command)) {
        switch (command.Command) {
        case SpiTesterCommand::GetDeviceInfo:
            SspSendWithChecksum(this->testerInfo);
            break;
        case SpiTesterCommand::CaptureNextTransfer:
            this->transferInfo = CaptureTransfer(command);
            break;
        case SpiTesterCommand::GetTransferInfo:
            SspSendWithChecksum(this->transferInfo);
            break;
        case SpiTesterCommand::StartPeriodicInterrupts:
            this->interruptInfo = RunPeriodicInterrupts(command);
            break;
        case SpiTesterCommand::GetPeriodicInterruptInfo:
            SspSendWithChecksum(this->interruptInfo);
            break;
        default:
            // invalid command
            break;
        }
    }
}