#pragma once

/// @file
/// @brief STM32F405 register map, written by hand from RM0090. Only the
///        peripherals and registers actually used are defined. Register
///        names follow the reference manual on purpose, not the project
///        naming convention: grep-ability against RM0090 wins here.

#include <cstdint>

namespace mark4
{
    // NOLINTBEGIN(readability-identifier-naming): register and field names
    // match RM0090 verbatim, the reference this file mirrors.
    /// Reset and clock control (RM0090 section 6).
    struct RccRegisters
    {
        volatile std::uint32_t CR;       ///< clock control
        volatile std::uint32_t PLLCFGR;  ///< main PLL configuration
        volatile std::uint32_t CFGR;     ///< clock configuration
        volatile std::uint32_t CIR;      ///< clock interrupt
        volatile std::uint32_t AHB1RSTR; ///< AHB1 peripheral reset
        volatile std::uint32_t AHB2RSTR; ///< AHB2 peripheral reset
        volatile std::uint32_t AHB3RSTR; ///< AHB3 peripheral reset
        std::uint32_t RESERVED0;         ///< 0x1C
        volatile std::uint32_t APB1RSTR; ///< APB1 peripheral reset
        volatile std::uint32_t APB2RSTR; ///< APB2 peripheral reset
        std::uint32_t RESERVED1[2];      ///< 0x28, 0x2C
        volatile std::uint32_t AHB1ENR;  ///< AHB1 peripheral clock enable
        volatile std::uint32_t AHB2ENR;  ///< AHB2 peripheral clock enable
        volatile std::uint32_t AHB3ENR;  ///< AHB3 peripheral clock enable
        std::uint32_t RESERVED2;         ///< 0x3C
        volatile std::uint32_t APB1ENR;  ///< APB1 peripheral clock enable
        volatile std::uint32_t APB2ENR;  ///< APB2 peripheral clock enable
    };

    /// Embedded flash interface (RM0090 section 3): access control, then
    /// the program/erase controller the OTA store drives.
    struct FlashRegisters
    {
        volatile std::uint32_t ACR;     ///< access control (latency, caches)
        volatile std::uint32_t KEYR;    ///< unlock key register
        volatile std::uint32_t OPTKEYR; ///< option byte unlock key register
        volatile std::uint32_t SR;      ///< status (BSY, EOP, error flags)
        volatile std::uint32_t CR;      ///< control (PG, SER, SNB, PSIZE, STRT, LOCK)
        volatile std::uint32_t OPTCR;   ///< option byte control
    };

    /// General-purpose I/O port (RM0090 section 8).
    struct GpioRegisters
    {
        volatile std::uint32_t MODER;   ///< mode (input/output/AF/analog)
        volatile std::uint32_t OTYPER;  ///< output type (push-pull/open-drain)
        volatile std::uint32_t OSPEEDR; ///< output speed
        volatile std::uint32_t PUPDR;   ///< pull-up/pull-down
        volatile std::uint32_t IDR;     ///< input data
        volatile std::uint32_t ODR;     ///< output data
        volatile std::uint32_t BSRR;    ///< bit set/reset
        volatile std::uint32_t LCKR;    ///< configuration lock
        volatile std::uint32_t AFR[2];  ///< alternate function low/high
    };

    /// Inter-integrated circuit (RM0090 section 27).
    struct I2cRegisters
    {
        volatile std::uint32_t CR1;   ///< control 1 (PE, START, STOP, SWRST)
        volatile std::uint32_t CR2;   ///< control 2 (peripheral clock MHz)
        volatile std::uint32_t OAR1;  ///< own address 1
        volatile std::uint32_t OAR2;  ///< own address 2
        volatile std::uint32_t DR;    ///< data
        volatile std::uint32_t SR1;   ///< status 1 (SB, ADDR, AF, ...)
        volatile std::uint32_t SR2;   ///< status 2 (BUSY, MSL, ...)
        volatile std::uint32_t CCR;   ///< clock control (SCL timing)
        volatile std::uint32_t TRISE; ///< maximum rise time
        volatile std::uint32_t FLTR;  ///< noise filters
    };

    /// General-purpose timer, TIM2..TIM5 layout (RM0090 section 18).
    struct TimRegisters
    {
        volatile std::uint32_t CR1;   ///< control 1 (CEN)
        volatile std::uint32_t CR2;   ///< control 2
        volatile std::uint32_t SMCR;  ///< slave mode control
        volatile std::uint32_t DIER;  ///< interrupt enable (UIE)
        volatile std::uint32_t SR;    ///< status (UIF)
        volatile std::uint32_t EGR;   ///< event generation (UG)
        volatile std::uint32_t CCMR1; ///< capture/compare mode 1
        volatile std::uint32_t CCMR2; ///< capture/compare mode 2
        volatile std::uint32_t CCER;  ///< capture/compare enable
        volatile std::uint32_t CNT;   ///< counter
        volatile std::uint32_t PSC;   ///< prescaler
        volatile std::uint32_t ARR;   ///< auto-reload
        volatile std::uint32_t RCR;   ///< repetition counter
        volatile std::uint32_t CCR1;  ///< capture/compare 1
        volatile std::uint32_t CCR2;  ///< capture/compare 2
        volatile std::uint32_t CCR3;  ///< capture/compare 3
        volatile std::uint32_t CCR4;  ///< capture/compare 4
        volatile std::uint32_t BDTR;  ///< break and dead-time
        volatile std::uint32_t DCR;   ///< DMA control (DBA, DBL)
        volatile std::uint32_t DMAR;  ///< DMA address for full transfer
    };

    /// Universal synchronous/asynchronous receiver transmitter (RM0090
    /// section 30).
    struct UsartRegisters
    {
        volatile std::uint32_t SR;   ///< status (TXE, TC, RXNE)
        volatile std::uint32_t DR;   ///< data
        volatile std::uint32_t BRR;  ///< baud rate divisor
        volatile std::uint32_t CR1;  ///< control 1 (UE, TE, RE, TXEIE)
        volatile std::uint32_t CR2;  ///< control 2 (stop bits)
        volatile std::uint32_t CR3;  ///< control 3 (flow control, DMA)
        volatile std::uint32_t GTPR; ///< guard time and prescaler
    };

    /// Data watchpoint and trace unit (ARMv7-M, cycle counter only).
    struct DwtRegisters
    {
        volatile std::uint32_t CTRL;   ///< control (CYCCNTENA)
        volatile std::uint32_t CYCCNT; ///< free-running core cycle counter
    };

    /// One stream of a DMA controller (RM0090 section 10).
    struct DmaStreamRegisters
    {
        volatile std::uint32_t CR;   ///< configuration (EN, CHSEL, CIRC, MINC)
        volatile std::uint32_t NDTR; ///< number of data items left
        volatile std::uint32_t PAR;  ///< peripheral address
        volatile std::uint32_t M0AR; ///< memory 0 address
        volatile std::uint32_t M1AR; ///< memory 1 address (double buffer)
        volatile std::uint32_t FCR;  ///< FIFO control
    };

    /// Interrupt status and clear block of a DMA controller.
    struct DmaRegisters
    {
        volatile std::uint32_t LISR;  ///< status, streams 0-3
        volatile std::uint32_t HISR;  ///< status, streams 4-7
        volatile std::uint32_t LIFCR; ///< clear, streams 0-3
        volatile std::uint32_t HIFCR; ///< clear, streams 4-7
    };

    inline RccRegisters *const RCC = reinterpret_cast<RccRegisters *>(0x40023800U);
    inline FlashRegisters *const FLASH = reinterpret_cast<FlashRegisters *>(0x40023C00U);
    inline GpioRegisters *const GPIOA = reinterpret_cast<GpioRegisters *>(0x40020000U);
    inline GpioRegisters *const GPIOB = reinterpret_cast<GpioRegisters *>(0x40020400U);
    inline GpioRegisters *const GPIOC = reinterpret_cast<GpioRegisters *>(0x40020800U);
    inline I2cRegisters *const I2C1 = reinterpret_cast<I2cRegisters *>(0x40005400U);
    inline TimRegisters *const TIM2 = reinterpret_cast<TimRegisters *>(0x40000000U);
    inline TimRegisters *const TIM3 = reinterpret_cast<TimRegisters *>(0x40000400U);
    inline TimRegisters *const TIM6 = reinterpret_cast<TimRegisters *>(0x40001000U);
    inline UsartRegisters *const USART1 = reinterpret_cast<UsartRegisters *>(0x40011000U);
    inline DmaRegisters *const DMA1 = reinterpret_cast<DmaRegisters *>(0x40026000U);
    inline DmaRegisters *const DMA2 = reinterpret_cast<DmaRegisters *>(0x40026400U);

    /// TIM3_UP drives DMA1 stream 2, channel 5 (RM0090 table 42): one update
    /// event bursts the four CCRs through TIM3's DMAR window.
    inline DmaStreamRegisters *const DMA1_STREAM2 =
        reinterpret_cast<DmaStreamRegisters *>(0x40026000U + 0x10U + (2U * 0x18U));

    /// USART1_RX lives on DMA2 stream 2, channel 4 (RM0090 table 43).
    inline DmaStreamRegisters *const DMA2_STREAM2 =
        reinterpret_cast<DmaStreamRegisters *>(0x40026400U + 0x10U + (2U * 0x18U));
    inline DwtRegisters *const DWT = reinterpret_cast<DwtRegisters *>(0xE0001000U);

    /// Debug exception and monitor control register: TRCENA gates the DWT.
    inline volatile std::uint32_t *const DEMCR =
        reinterpret_cast<volatile std::uint32_t *>(0xE000EDFCU);

    /// NVIC interrupt set-enable registers, one bit per IRQ number.
    inline volatile std::uint32_t *const NVIC_ISER =
        reinterpret_cast<volatile std::uint32_t *>(0xE000E100U);

    /// Vector table offset register (SCB): the base address the core reads
    /// every exception vector from. Each image points it at its own table,
    /// which is what lets the same firmware run from either OTA slot.
    inline volatile std::uint32_t *const SCB_VTOR =
        reinterpret_cast<volatile std::uint32_t *>(0xE000ED08U);

    /// Application interrupt and reset control register (SCB): VECTKEY in
    /// the high half, SYSRESETREQ requests a system reset.
    inline volatile std::uint32_t *const SCB_AIRCR =
        reinterpret_cast<volatile std::uint32_t *>(0xE000ED0CU);
    // NOLINTEND(readability-identifier-naming)
} // namespace mark4
