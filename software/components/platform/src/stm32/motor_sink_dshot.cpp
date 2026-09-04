#include "platform_stm32/motor_sink_dshot.hpp"

#include <cstddef>
#include <cstdint>

#include <stm32f405xx.h>

namespace mark4
{
    namespace
    {
        // Motor pins: PA6/PA7 = TIM3_CH1/CH2, PB0/PB1 = TIM3_CH3/CH4, all AF2.
        constexpr std::uint32_t GPIO_MODER_AF = 0x2U;     // alternate function
        constexpr std::uint32_t GPIO_OSPEED_VHIGH = 0x3U; // clean 600 kHz edges
        constexpr std::uint32_t GPIO_AF2 = 0x2U;

        // TIM3 in PWM mode 1 on the four channels, CCR preloaded (OCxPE).
        constexpr std::uint32_t TIM_CCMR_PWM1_LOW = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | //
                                                    TIM_CCMR1_OC1PE;
        constexpr std::uint32_t TIM_CCMR_PWM1_HIGH = TIM_CCMR1_OC2M_2 | TIM_CCMR1_OC2M_1 | //
                                                     TIM_CCMR1_OC2PE;
        constexpr std::uint32_t TIM_CCMR_PWM1 = TIM_CCMR_PWM1_LOW | TIM_CCMR_PWM1_HIGH;
        constexpr std::uint32_t TIM_CCER_ALL =
            TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC3E | TIM_CCER_CC4E;

        // TIM3 DMA burst window: base at CCR1 (offset 0x34 / 4 = 13), four
        // transfers per request (DBL = count - 1).
        constexpr std::uint32_t TIM_DCR_BURST =
            ((DSHOT_MOTORS - 1U) << TIM_DCR_DBL_Pos) | (13U << TIM_DCR_DBA_Pos);

        // DMA1 stream 2 to TIM3_UP: channel 5, memory-to-peripheral, 16-bit,
        // memory-incrementing, single-shot (no CIRC).
        constexpr std::uint32_t DMA_CR_CONFIG = (5U << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MSIZE_0 |
                                                DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_DIR_0;

        // Interrupt/error flags of stream 2 in the low status register.
        constexpr std::uint32_t DMA_LIFCR_STREAM2 = DMA_LIFCR_CTCIF2 | DMA_LIFCR_CHTIF2 |
                                                    DMA_LIFCR_CTEIF2 | DMA_LIFCR_CDMEIF2 |
                                                    DMA_LIFCR_CFEIF2;

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
        TIM3->DCR = TIM_DCR_BURST;
        TIM3->DIER = TIM_DIER_UDE; // update raises a DMA request
        TIM3->CR1 = TIM_CR1_ARPE;
        TIM3->EGR = TIM_EGR_UG; // latch PSC/ARR and the zeroed CCR preloads
        TIM3->SR = 0U;
        TIM3->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN; // free-running from here

        // DMA1 stream 2 addresses are fixed; only NDTR and EN are touched per
        // frame. The buffer is memory 0, TIM3's DMAR window the peripheral.
        // TIM3_UP drives DMA1 stream 2, channel 5 (RM0090 table 42): one
        // update event bursts the four CCRs through TIM3's DMAR window.
        DMA1_Stream2->CR = DMA_CR_CONFIG;
        DMA1_Stream2->PAR = reinterpret_cast<std::uint32_t>(&TIM3->DMAR);
        DMA1_Stream2->M0AR = reinterpret_cast<std::uint32_t>(m_buffer.data());
    }

    void MotorSinkDshot::push(const mark4::ActuatorFrame &frame)
    {
        // At 500 Hz the 30 us transfer is always long done; the guard only
        // matters if the loop rate ever changes.
        // TODO(tmagne): surface m_skipped (status log line) once it can occur.
        // A skipped frame drops its telemetry request too: the sequencer
        // behind it times out and moves on, which is the right outcome for
        // a request that never reached the wire.
        const std::size_t telemetryMotor = m_telemetryMotor;
        m_telemetryMotor = DSHOT_MOTORS;
        if ((DMA1_Stream2->CR & DMA_SxCR_EN) != 0U)
        {
            ++m_skipped;
            return;
        }

        m_last = frame;
        for (std::size_t motor = 0U; motor < DSHOT_MOTORS; ++motor)
        {
            const std::uint16_t value = dshotThrottle(frame.motor[motor]);
            const std::uint16_t encoded = dshotFrame(value, motor == telemetryMotor);
            dshotExpand(encoded, &m_buffer[motor], DSHOT_MOTORS);
        }

        DMA1->LIFCR = DMA_LIFCR_STREAM2;
        DMA1_Stream2->NDTR = DSHOT_BUFFER;
        DMA1_Stream2->CR = DMA_CR_CONFIG | DMA_SxCR_EN;
    }
} // namespace mark4
