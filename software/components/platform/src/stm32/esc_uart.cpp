#include "platform_stm32/esc_uart.hpp"

#include <cstdint>

#include <stm32f405xx.h>

#include "platform_stm32/board.hpp"
#include "platform_stm32/esc_telemetry.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t RX_PIN = 3U; ///< PA3
        constexpr std::uint32_t GPIO_AF7 = 7U;
        constexpr std::uint32_t GPIO_MODER_AF = 2U;
        constexpr std::uint32_t GPIO_PUPDR_UP = 1U;

        /// USART2_RX lives on DMA1 stream 5, channel 4 (RM0090 table 42).
        constexpr std::uint32_t DMA_SXCR_CHSEL_4 = 4U << DMA_SxCR_CHSEL_Pos;

        /// Every interrupt flag of DMA1 stream 5 in HIFCR (RM0090 10.5.4).
        constexpr std::uint32_t DMA_HIFCR_STREAM5_ALL = DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 |
                                                        DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 |
                                                        DMA_HIFCR_CFEIF5;

        /// Receive buffer, filled by a circular DMA and drained once per
        /// output frame: an ESC answers at most one 10-byte frame per
        /// request and there is one request outstanding, so the ring only
        /// has to hold a frame or two of slack. Must be a power of two.
        constexpr std::uint32_t RX_RING_SIZE = 64U;
        constexpr std::uint32_t RX_RING_MASK = RX_RING_SIZE - 1U;
        static_assert((RX_RING_SIZE & RX_RING_MASK) == 0U);
        static_assert(RX_RING_SIZE >= 2U * KISS_FRAME_SIZE);

        /// DMA target: .bss lands in SRAM, which the DMA reaches (CCM would
        /// not).
        std::uint8_t g_rxRing[RX_RING_SIZE];

        /// Next receive buffer byte to hand out, modulo the ring; the DMA
        /// write position is derived from NDTR, so only the reader keeps
        /// state.
        std::uint32_t g_rxTail = 0U;

        std::uint32_t g_rxDrops = 0U;

        bool g_initialized = false;
    } // namespace

    bool escUartInit()
    {
        if (g_initialized)
        {
            return true;
        }

        RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_DMA1EN;
        RCC->APB1ENR = RCC->APB1ENR | RCC_APB1ENR_USART2EN;

        // PA3 as USART2_RX, pulled up: the line idles high, and an ESC
        // that is not powered must not leave it floating into the receiver.
        constexpr std::uint32_t AF_MASK = 0xFU << (4U * RX_PIN);
        constexpr std::uint32_t AF_USART = GPIO_AF7 << (4U * RX_PIN);
        GPIOA->AFR[0] = (GPIOA->AFR[0] & ~AF_MASK) | AF_USART;
        constexpr std::uint32_t PIN2_MASK = 3U << (2U * RX_PIN);
        GPIOA->PUPDR = (GPIOA->PUPDR & ~PIN2_MASK) | (GPIO_PUPDR_UP << (2U * RX_PIN));
        GPIOA->MODER = (GPIOA->MODER & ~PIN2_MASK) | (GPIO_MODER_AF << (2U * RX_PIN));

        // Oversampling by 16 (reset default): BRR is the clock divided by
        // the baud rate, mantissa and fraction packed as one. At 115200 the
        // truncation lands on 115385 baud, +0.16 %.
        USART2->BRR = APB1_CLOCK_HZ / ESC_TELEMETRY_BAUD_RATE;
        USART2->CR1 = USART_CR1_UE | USART_CR1_RE;

        // Circular DMA into g_rxRing, byte-wide both ends, started before
        // DMAR so no request is ever unanswered. A stale data byte is
        // flushed first, or it would be the stream's first.
        DMA1_Stream5->CR = 0U;
        while ((DMA1_Stream5->CR & DMA_SxCR_EN) != 0U)
        {
        }
        DMA1->HIFCR = DMA_HIFCR_STREAM5_ALL;
        static_cast<void>(USART2->SR);
        static_cast<void>(USART2->DR);
        DMA1_Stream5->PAR = reinterpret_cast<std::uint32_t>(&USART2->DR);
        DMA1_Stream5->M0AR = reinterpret_cast<std::uint32_t>(&g_rxRing[0]);
        DMA1_Stream5->NDTR = RX_RING_SIZE;
        DMA1_Stream5->CR = DMA_SXCR_CHSEL_4 | DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_EN;
        USART2->CR3 = USART2->CR3 | USART_CR3_DMAR;

        g_initialized = true;
        return true;
    }

    bool escUartRxPop(std::uint8_t &byteOut)
    {
        // An overrun means the DMA lost the wire for a moment; clearing it
        // is the SR-then-DR read sequence, and the DR read hands the
        // pending byte to the CPU instead of the DMA, acceptable in an
        // already-lossy spot. Framing and noise errors are cleared the same
        // way and the corrupted byte is left to the frame CRC.
        if ((USART2->SR & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0U)
        {
            static_cast<void>(USART2->DR);
            ++g_rxDrops;
        }

        const std::uint32_t head = (RX_RING_SIZE - DMA1_Stream5->NDTR) & RX_RING_MASK;
        if (g_rxTail == head)
        {
            return false;
        }
        byteOut = g_rxRing[g_rxTail];
        g_rxTail = (g_rxTail + 1U) & RX_RING_MASK;
        return true;
    }

    std::uint32_t escUartRxDrops()
    {
        return g_rxDrops;
    }
} // namespace mark4
