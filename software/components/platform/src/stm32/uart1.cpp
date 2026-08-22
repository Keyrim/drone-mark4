#include "platform_stm32/uart1.hpp"

#include <cstdint>

#include "registers.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t RCC_AHB1ENR_GPIOBEN = 1U << 1U;
        constexpr std::uint32_t RCC_APB2ENR_USART1EN = 1U << 4U;

        constexpr std::uint32_t TX_PIN = 6U; ///< PB6
        constexpr std::uint32_t RX_PIN = 7U; ///< PB7
        constexpr std::uint32_t GPIO_AF7 = 7U;
        constexpr std::uint32_t GPIO_MODER_AF = 2U;

        constexpr std::uint32_t USART_CR1_UE = 1U << 13U;
        constexpr std::uint32_t USART_CR1_TE = 1U << 3U;
        constexpr std::uint32_t USART_CR1_RE = 1U << 2U;
        constexpr std::uint32_t USART_CR1_TXEIE = 1U << 7U;
        constexpr std::uint32_t USART_CR1_RXNEIE = 1U << 5U;
        constexpr std::uint32_t USART_SR_TXE = 1U << 7U;
        constexpr std::uint32_t USART_SR_RXNE = 1U << 5U;
        constexpr std::uint32_t USART_SR_ORE = 1U << 3U;

        constexpr std::uint32_t APB2_CLOCK_HZ = 84000000U;
        constexpr std::uint32_t USART1_IRQ_NUMBER = 37U;

        /// Transmit ring capacity; must be a power of two. Holds about
        /// 45 ms of a saturated line (blackbox records plus telemetry),
        /// the drop policy of the senders covers the rest.
        constexpr std::uint32_t TX_RING_SIZE = 4096U;
        constexpr std::uint32_t TX_RING_MASK = TX_RING_SIZE - 1U;
        static_assert((TX_RING_SIZE & TX_RING_MASK) == 0U);

        /// Receive ring capacity; must be a power of two. The uplink is a
        /// handful of small command frames per second, drained every
        /// flight loop tick: 256 bytes is already a large margin.
        constexpr std::uint32_t RX_RING_SIZE = 256U;
        constexpr std::uint32_t RX_RING_MASK = RX_RING_SIZE - 1U;
        static_assert((RX_RING_SIZE & RX_RING_MASK) == 0U);

        std::uint8_t g_txRing[TX_RING_SIZE];
        std::uint8_t g_rxRing[RX_RING_SIZE];

        /// Written by uart1TxPush() only; read by the interrupt handler.
        volatile std::uint32_t g_txHead = 0U;

        /// Written by the interrupt handler only; read by uart1TxPush().
        volatile std::uint32_t g_txTail = 0U;

        /// Written by the interrupt handler only; read by uart1RxPop().
        volatile std::uint32_t g_rxHead = 0U;

        /// Written by uart1RxPop() only; read by the interrupt handler.
        volatile std::uint32_t g_rxTail = 0U;

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
        USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

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
        if (g_rxTail == g_rxHead)
        {
            return false;
        }
        byteOut = g_rxRing[g_rxTail & RX_RING_MASK];
        __asm volatile("" ::: "memory");
        g_rxTail = g_rxTail + 1U;
        return true;
    }
} // namespace mark4

/// USART1 interrupt: stores the received byte (a full ring drops it, the
/// serial framing resynchronizes downstream), feeds the transmit register
/// from its ring, and stops asking for TXE (TXEIE off) once it drains.
extern "C" void USART1_IRQHandler(void)
{
    const std::uint32_t sr = mark4::USART1->SR;

    if ((sr & (mark4::USART_SR_RXNE | mark4::USART_SR_ORE)) != 0U)
    {
        // Reading DR clears RXNE, and clears an overrun once SR was read.
        const auto byte = static_cast<std::uint8_t>(mark4::USART1->DR);
        const std::uint32_t used = mark4::g_rxHead - mark4::g_rxTail;
        if (used < mark4::RX_RING_SIZE - 1U)
        {
            mark4::g_rxRing[mark4::g_rxHead & mark4::RX_RING_MASK] = byte;
            mark4::g_rxHead = mark4::g_rxHead + 1U;
        }
    }

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
