#include "platform_stm32/telemetry_sender_stm32.hpp"

#include <cstdint>

#include "protocol/serial_framing.hpp"
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
        constexpr std::uint32_t USART_SR_TXE = 1U << 7U;

        constexpr std::uint32_t APB2_CLOCK_HZ = 84000000U;
        constexpr std::uint32_t USART1_IRQ_NUMBER = 37U;

        /// Ring capacity; must be a power of two. Holds about 45 ms of a
        /// saturated line (blackbox records plus telemetry), the drop
        /// policy covers the rest.
        constexpr std::uint32_t RING_SIZE = 4096U;
        constexpr std::uint32_t RING_MASK = RING_SIZE - 1U;
        static_assert((RING_SIZE & RING_MASK) == 0U);

        std::uint8_t g_ring[RING_SIZE];

        /// Written by send() only; read by the interrupt handler.
        volatile std::uint32_t g_head = 0U;

        /// Written by the interrupt handler only; read by send().
        volatile std::uint32_t g_tail = 0U;
    } // namespace

    bool TelemetrySenderStm32::init()
    {
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
        USART1->BRR = APB2_CLOCK_HZ / BAUD_RATE;
        USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

        NVIC_ISER[USART1_IRQ_NUMBER / 32U] = 1U << (USART1_IRQ_NUMBER % 32U);
        return true;
    }

    void TelemetrySenderStm32::send(const std::uint8_t *data, std::size_t size)
    {
        std::uint8_t frame[SERIAL_MAX_PAYLOAD + SERIAL_FRAME_OVERHEAD];
        const std::size_t framed = encodeSerialFrame(data, size, frame);
        if (framed == 0U)
        {
            ++m_packetsDropped;
            return;
        }

        const std::uint32_t head = g_head;
        const std::uint32_t used = head - g_tail;
        if (framed > (RING_SIZE - 1U) - used)
        {
            ++m_packetsDropped;
            return;
        }
        for (std::uint32_t index = 0U; index < framed; ++index)
        {
            g_ring[(head + index) & RING_MASK] = frame[index];
        }
        // The bytes must be visible before the head moves: the interrupt
        // handler reads up to head.
        __asm volatile("" ::: "memory");
        g_head = head + framed;
        ++m_packetsSent;

        USART1->CR1 = USART1->CR1 | USART_CR1_TXEIE;
    }
} // namespace mark4

/// USART1 interrupt: feeds the transmit register from the ring, and goes
/// back to sleep (TXEIE off) once the ring drains.
extern "C" void USART1_IRQHandler(void)
{
    if ((mark4::USART1->SR & mark4::USART_SR_TXE) == 0U)
    {
        return;
    }
    if (mark4::g_tail == mark4::g_head)
    {
        mark4::USART1->CR1 = mark4::USART1->CR1 & ~mark4::USART_CR1_TXEIE;
        return;
    }
    mark4::USART1->DR = mark4::g_ring[mark4::g_tail & mark4::RING_MASK];
    mark4::g_tail = mark4::g_tail + 1U;
}
