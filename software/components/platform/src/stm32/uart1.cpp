#include "platform_stm32/uart1.hpp"

#include <cstdint>

#include "registers.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t RCC_AHB1ENR_GPIOBEN = 1U << 1U;
        constexpr std::uint32_t RCC_AHB1ENR_DMA2EN = 1U << 22U;
        constexpr std::uint32_t RCC_APB2ENR_USART1EN = 1U << 4U;

        constexpr std::uint32_t TX_PIN = 6U; ///< PB6
        constexpr std::uint32_t RX_PIN = 7U; ///< PB7
        constexpr std::uint32_t GPIO_AF7 = 7U;
        constexpr std::uint32_t GPIO_MODER_AF = 2U;

        constexpr std::uint32_t USART_CR1_UE = 1U << 13U;
        constexpr std::uint32_t USART_CR1_TE = 1U << 3U;
        constexpr std::uint32_t USART_CR1_RE = 1U << 2U;
        constexpr std::uint32_t USART_CR1_TXEIE = 1U << 7U;
        constexpr std::uint32_t USART_SR_TXE = 1U << 7U;
        constexpr std::uint32_t USART_SR_ORE = 1U << 3U;
        constexpr std::uint32_t USART_CR3_DMAR = 1U << 6U;

        constexpr std::uint32_t DMA_SXCR_EN = 1U << 0U;
        constexpr std::uint32_t DMA_SXCR_CIRC = 1U << 8U;
        constexpr std::uint32_t DMA_SXCR_MINC = 1U << 10U;
        constexpr std::uint32_t DMA_SXCR_CHSEL_4 = 4U << 25U;

        /// Every interrupt flag of DMA2 stream 2 in LIFCR (RM0090 10.5.2).
        constexpr std::uint32_t DMA_LIFCR_STREAM2_ALL = 0x3DU << 16U;

        constexpr std::uint32_t APB2_CLOCK_HZ = 84000000U;
        constexpr std::uint32_t USART1_IRQ_NUMBER = 37U;

        /// Transmit ring capacity; must be a power of two. Holds about
        /// 45 ms of a saturated line (blackbox records plus telemetry),
        /// the drop policy of the senders covers the rest.
        constexpr std::uint32_t TX_RING_SIZE = 4096U;
        constexpr std::uint32_t TX_RING_MASK = TX_RING_SIZE - 1U;
        static_assert((TX_RING_SIZE & TX_RING_MASK) == 0U);

        /// Receive buffer, filled by a circular DMA (docs/ota-design.md
        /// section 2): flash program and erase stall every flash-resident
        /// instruction fetch, receive interrupt included, but the DMA only
        /// touches SRAM and the USART and rides straight through - the
        /// bench proved a byte-interrupt RX loses part of every chunk that
        /// streams in while the previous one programs. Must be a power of
        /// two; 2048 is ~22 ms of a saturated line, and the poll loops run
        /// far faster than that. The one window with nobody draining is a
        /// slot erase inside OTA_BEGIN (seconds), during which the ground
        /// side sends nothing but keepalives, which fit many times over.
        constexpr std::uint32_t RX_RING_SIZE = 2048U;
        constexpr std::uint32_t RX_RING_MASK = RX_RING_SIZE - 1U;
        static_assert((RX_RING_SIZE & RX_RING_MASK) == 0U);

        std::uint8_t g_txRing[TX_RING_SIZE];
        std::uint8_t g_rxRing[RX_RING_SIZE];

        /// Written by uart1TxPush() only; read by the interrupt handler.
        volatile std::uint32_t g_txHead = 0U;

        /// Written by the interrupt handler only; read by uart1TxPush().
        volatile std::uint32_t g_txTail = 0U;

        /// Next receive buffer byte to hand out, modulo the ring; the DMA
        /// write position is derived from NDTR, so only the reader keeps
        /// state. Written by uart1RxPop() only.
        std::uint32_t g_rxTail = 0U;

        /// Overrun events seen while draining (the DMA fell behind the
        /// wire, or the reader fell a whole lap behind the DMA - neither
        /// is expected off the bench).
        volatile std::uint32_t g_rxDrops = 0U;

        bool g_initialized = false;
    } // namespace

    bool uart1Init()
    {
        if (g_initialized)
        {
            return true;
        }

        RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_GPIOBEN;
        RCC->APB2ENR = RCC->APB2ENR | RCC_APB2ENR_USART1EN;

        constexpr std::uint32_t AF_MASK = (0xFU << (4U * TX_PIN)) | (0xFU << (4U * RX_PIN));
        constexpr std::uint32_t AF_USART =
            (GPIO_AF7 << (4U * TX_PIN)) | (GPIO_AF7 << (4U * RX_PIN));
        GPIOB->AFR[0] = (GPIOB->AFR[0] & ~AF_MASK) | AF_USART;

        constexpr std::uint32_t MODE_MASK = (3U << (2U * TX_PIN)) | (3U << (2U * RX_PIN));
        constexpr std::uint32_t MODE_AF =
            (GPIO_MODER_AF << (2U * TX_PIN)) | (GPIO_MODER_AF << (2U * RX_PIN));
        GPIOB->MODER = (GPIOB->MODER & ~MODE_MASK) | MODE_AF;

        // Oversampling by 16 (reset default): BRR is simply the clock
        // divided by the baud rate, mantissa and fraction packed as one.
        // At 921600 the truncation lands on 923077 baud, +0.16 % - well
        // inside the USART tolerance.
        USART1->BRR = APB2_CLOCK_HZ / UART1_BAUD_RATE;
        USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

        // Receive side: circular DMA into g_rxRing, byte-wide both ends,
        // started before DMAR so no request is ever unanswered. A stale
        // data byte is flushed first, or it would be the stream's first.
        RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_DMA2EN;
        DMA2_STREAM2->CR = 0U;
        while ((DMA2_STREAM2->CR & DMA_SXCR_EN) != 0U)
        {
        }
        DMA2->LIFCR = DMA_LIFCR_STREAM2_ALL;
        static_cast<void>(USART1->SR);
        static_cast<void>(USART1->DR);
        DMA2_STREAM2->PAR = reinterpret_cast<std::uint32_t>(&USART1->DR);
        DMA2_STREAM2->M0AR = reinterpret_cast<std::uint32_t>(&g_rxRing[0]);
        DMA2_STREAM2->NDTR = RX_RING_SIZE;
        DMA2_STREAM2->CR = DMA_SXCR_CHSEL_4 | DMA_SXCR_MINC | DMA_SXCR_CIRC | DMA_SXCR_EN;
        USART1->CR3 = USART1->CR3 | USART_CR3_DMAR;

        // The interrupt only feeds the transmitter now.
        NVIC_ISER[USART1_IRQ_NUMBER / 32U] = 1U << (USART1_IRQ_NUMBER % 32U);
        g_initialized = true;
        return true;
    }

    bool uart1TxPush(const std::uint8_t *data, std::size_t size)
    {
        const std::uint32_t head = g_txHead;
        const std::uint32_t used = head - g_txTail;
        if (size > (TX_RING_SIZE - 1U) - used)
        {
            return false;
        }
        for (std::uint32_t index = 0U; index < size; ++index)
        {
            g_txRing[(head + index) & TX_RING_MASK] = data[index];
        }
        // The bytes must be visible before the head moves: the interrupt
        // handler reads up to head.
        __asm volatile("" ::: "memory");
        g_txHead = head + static_cast<std::uint32_t>(size);

        USART1->CR1 = USART1->CR1 | USART_CR1_TXEIE;
        return true;
    }

    bool uart1RxPop(std::uint8_t &byteOut)
    {
        // An overrun means the DMA lost the wire for a moment (it should
        // not happen; the counter is bench forensics). Clearing it is the
        // SR-then-DR read sequence; the DR read hands the pending byte to
        // the CPU instead of the DMA, acceptable in an already-lossy spot.
        if ((USART1->SR & USART_SR_ORE) != 0U)
        {
            static_cast<void>(USART1->DR);
            g_rxDrops = g_rxDrops + 1U;
        }

        const std::uint32_t head = (RX_RING_SIZE - DMA2_STREAM2->NDTR) & RX_RING_MASK;
        if (g_rxTail == head)
        {
            return false;
        }
        byteOut = g_rxRing[g_rxTail];
        g_rxTail = (g_rxTail + 1U) & RX_RING_MASK;
        return true;
    }

    std::uint32_t uart1RxDrops()
    {
        return g_rxDrops;
    }
} // namespace mark4

/// USART1 interrupt: feeds the transmit register from its ring and stops
/// asking for TXE (TXEIE off) once it drains. Reception never comes here,
/// the circular DMA owns it.
extern "C" void USART1_IRQHandler(void)
{
    const std::uint32_t sr = mark4::USART1->SR;

    if ((mark4::USART1->CR1 & mark4::USART_CR1_TXEIE) != 0U && (sr & mark4::USART_SR_TXE) != 0U)
    {
        if (mark4::g_txTail == mark4::g_txHead)
        {
            mark4::USART1->CR1 = mark4::USART1->CR1 & ~mark4::USART_CR1_TXEIE;
        }
        else
        {
            mark4::USART1->DR = mark4::g_txRing[mark4::g_txTail & mark4::TX_RING_MASK];
            mark4::g_txTail = mark4::g_txTail + 1U;
        }
    }
}
