#include "platform_stm32/motor_sink_dshot.hpp"

#include <cstddef>
#include <cstdint>

#include "registers.hpp"

namespace mark4
{
    namespace
    {
        // Clock enables.
        constexpr std::uint32_t RCC_AHB1ENR_GPIOAEN = 1U << 0U;
        constexpr std::uint32_t RCC_AHB1ENR_GPIOBEN = 1U << 1U;
        constexpr std::uint32_t RCC_AHB1ENR_DMA1EN = 1U << 21U;
        constexpr std::uint32_t RCC_APB1ENR_TIM3EN = 1U << 1U;

        // Motor pins: PA6/PA7 = TIM3_CH1/CH2, PB0/PB1 = TIM3_CH3/CH4, all AF2.
        constexpr std::uint32_t GPIO_MODER_AF = 0x2U;     // alternate function
        constexpr std::uint32_t GPIO_OSPEED_VHIGH = 0x3U; // clean 600 kHz edges
        constexpr std::uint32_t GPIO_AF2 = 0x2U;

        // TIM3 in PWM mode 1 on the four channels, CCR preloaded (OCxPE).
        constexpr std::uint32_t TIM_CCMR_PWM1_LOW = (0x6U << 4U) | (1U << 3U);
        constexpr std::uint32_t TIM_CCMR_PWM1_HIGH = (0x6U << 12U) | (1U << 11U);
        constexpr std::uint32_t TIM_CCMR_PWM1 = TIM_CCMR_PWM1_LOW | TIM_CCMR_PWM1_HIGH;
        constexpr std::uint32_t TIM_CCER_ALL = (1U << 0U) | (1U << 4U) | (1U << 8U) | (1U << 12U);
        constexpr std::uint32_t TIM_CR1_ARPE = 1U << 7U;
        constexpr std::uint32_t TIM_CR1_CEN = 1U << 0U;
        constexpr std::uint32_t TIM_EGR_UG = 1U << 0U;
        constexpr std::uint32_t TIM_DIER_UDE = 1U << 8U; // update raises a DMA request

        // TIM3 DMA burst window: base at CCR1 (offset 0x34 / 4 = 13), four
        // transfers per request (DBL = count - 1).
        constexpr std::uint32_t TIM_DCR_DBA = 13U;
        constexpr std::uint32_t TIM_DCR_DBL = (DSHOT_MOTORS - 1U) << 8U;

        // DMA1 stream 2 to TIM3_UP: channel 5, memory-to-peripheral, 16-bit,
        // memory-incrementing, single-shot (no CIRC).
        constexpr std::uint32_t DMA_CR_EN = 1U << 0U;
        constexpr std::uint32_t DMA_CR_DIR_M2P = 1U << 6U;
        constexpr std::uint32_t DMA_CR_MINC = 1U << 10U;
        constexpr std::uint32_t DMA_CR_PSIZE_HALF = 1U << 11U;
        constexpr std::uint32_t DMA_CR_MSIZE_HALF = 1U << 13U;
        constexpr std::uint32_t DMA_CR_CHSEL5 = 5U << 25U;
        constexpr std::uint32_t DMA_CR_CONFIG =
            DMA_CR_CHSEL5 | DMA_CR_MSIZE_HALF | DMA_CR_PSIZE_HALF | DMA_CR_MINC | DMA_CR_DIR_M2P;

        // Interrupt/error flags of stream 2 in the low status register.
        constexpr std::uint32_t DMA_LIFCR_STREAM2 =
            (1U << 16U) | (1U << 18U) | (1U << 19U) | (1U << 20U) | (1U << 21U);

        /// @brief Two-bit field write at a pin's position in a MODER-style
        ///        register (MODER, OSPEEDR).
        void setPin2(volatile std::uint32_t &reg, std::uint32_t pin, std::uint32_t value)
        {
            const std::uint32_t shift = 2U * pin;
            reg = (reg & ~(0x3U << shift)) | (value << shift);
        }

        /// @brief Four-bit alternate-function selector for a pin in AFR[0]
        ///        (pins 0-7).
        void setAfLow(volatile std::uint32_t &reg, std::uint32_t pin, std::uint32_t af)
        {
            const std::uint32_t shift = 4U * pin;
            reg = (reg & ~(0xFU << shift)) | (af << shift);
        }
    } // namespace

    void MotorSinkDshot::init()
    {
        RCC->AHB1ENR =
            RCC->AHB1ENR | RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_DMA1EN;
        RCC->APB1ENR = RCC->APB1ENR | RCC_APB1ENR_TIM3EN;

        // PA6, PA7 -> TIM3_CH1/CH2; PB0, PB1 -> TIM3_CH3/CH4, all AF2, fast.
        setPin2(GPIOA->MODER, 6U, GPIO_MODER_AF);
        setPin2(GPIOA->MODER, 7U, GPIO_MODER_AF);
        setPin2(GPIOA->OSPEEDR, 6U, GPIO_OSPEED_VHIGH);
        setPin2(GPIOA->OSPEEDR, 7U, GPIO_OSPEED_VHIGH);
        setAfLow(GPIOA->AFR[0], 6U, GPIO_AF2);
        setAfLow(GPIOA->AFR[0], 7U, GPIO_AF2);
        setPin2(GPIOB->MODER, 0U, GPIO_MODER_AF);
        setPin2(GPIOB->MODER, 1U, GPIO_MODER_AF);
        setPin2(GPIOB->OSPEEDR, 0U, GPIO_OSPEED_VHIGH);
        setPin2(GPIOB->OSPEEDR, 1U, GPIO_OSPEED_VHIGH);
        setAfLow(GPIOB->AFR[0], 0U, GPIO_AF2);
        setAfLow(GPIOB->AFR[0], 1U, GPIO_AF2);

        // TIM3: 84 MHz timer clock straight (PSC = 0), 140-tick bit period.
        TIM3->PSC = 0U;
        TIM3->ARR = DSHOT_ARR;
        TIM3->CCMR1 = TIM_CCMR_PWM1;
        TIM3->CCMR2 = TIM_CCMR_PWM1;
        TIM3->CCER = TIM_CCER_ALL;
        TIM3->CCR1 = 0U;
        TIM3->CCR2 = 0U;
        TIM3->CCR3 = 0U;
        TIM3->CCR4 = 0U;
        TIM3->DCR = TIM_DCR_DBL | TIM_DCR_DBA;
        TIM3->DIER = TIM_DIER_UDE;
        TIM3->CR1 = TIM_CR1_ARPE;
        TIM3->EGR = TIM_EGR_UG; // latch PSC/ARR and the zeroed CCR preloads
        TIM3->SR = 0U;
        TIM3->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN; // free-running from here

        // DMA1 stream 2 addresses are fixed; only NDTR and EN are touched per
        // frame. The buffer is memory 0, TIM3's DMAR window the peripheral.
        DMA1_STREAM2->CR = DMA_CR_CONFIG;
        DMA1_STREAM2->PAR = reinterpret_cast<std::uint32_t>(&TIM3->DMAR);
        DMA1_STREAM2->M0AR = reinterpret_cast<std::uint32_t>(m_buffer.data());
    }

    void MotorSinkDshot::push(const mark4::ActuatorFrame &frame)
    {
        // At 500 Hz the 30 us transfer is always long done; the guard only
        // matters if the loop rate ever changes.
        // TODO(tmagne): surface m_skipped (status log line) once it can occur.
        if ((DMA1_STREAM2->CR & DMA_CR_EN) != 0U)
        {
            ++m_skipped;
            return;
        }

        m_last = frame;
        for (std::size_t motor = 0U; motor < DSHOT_MOTORS; ++motor)
        {
            const std::uint16_t value = dshotThrottle(frame.motor[motor]);
            const std::uint16_t encoded = dshotFrame(value, false);
            dshotExpand(encoded, &m_buffer[motor], DSHOT_MOTORS);
        }

        DMA1->LIFCR = DMA_LIFCR_STREAM2;
        DMA1_STREAM2->NDTR = DSHOT_BUFFER;
        DMA1_STREAM2->CR = DMA_CR_CONFIG | DMA_CR_EN;
    }
} // namespace mark4
