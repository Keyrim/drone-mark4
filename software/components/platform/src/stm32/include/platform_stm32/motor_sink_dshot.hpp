#pragma once

/// @file
/// @brief DShot600 motor sink: drives four ESC channels off TIM3 CH1-4
///        through one burst-DMA stream. The frame encoding is pure and
///        lives here so a desktop test pins it without any hardware.

#include <array>
#include <cmath>
#include <cstdint>

#include "platform/motor_sink.hpp"

namespace mark4
{
    /// DShot600 on the 84 MHz APB1 timer clock: a 1667 ns bit period, with
    /// the high time carrying the bit (625 ns for a 0, 1250 ns for a 1).
    constexpr std::uint32_t DSHOT_ARR = 139U; ///< bit period 140 ticks, ARR = period - 1
    constexpr std::uint16_t DSHOT_T0H = 52U;  ///< 625 ns high: a 0 bit
    constexpr std::uint16_t DSHOT_T1H = 105U; ///< 1250 ns high: a 1 bit

    /// One frame is 16 bits MSB first, then two slots parked low so the four
    /// lines idle low before the burst stops (18 slots x 4 channels).
    constexpr std::size_t DSHOT_BITS = 16U;
    constexpr std::size_t DSHOT_TRAILING = 2U;
    constexpr std::size_t DSHOT_SLOTS = DSHOT_BITS + DSHOT_TRAILING;
    constexpr std::size_t DSHOT_MOTORS = 4U;
    constexpr std::size_t DSHOT_BUFFER = DSHOT_SLOTS * DSHOT_MOTORS;

    /// DShot value space: 0 stops the motor, 1-47 are ESC commands (unused),
    /// 48-2047 is the throttle range.
    constexpr std::uint16_t DSHOT_THROTTLE_MIN = 48U;
    constexpr std::uint16_t DSHOT_THROTTLE_MAX = 2047U;

    /// @brief Maps a normalized command to an 11-bit DShot value, stateless.
    ///        A command at or below zero stops the motor; otherwise it spans
    ///        the throttle range, clamped to the top.
    /// @param motor normalized command, nominally [0, 1]
    /// @return 0 (stop) or 48..2047 (throttle)
    [[nodiscard]] inline std::uint16_t dshotThrottle(float motor)
    {
        if (motor <= 0.0f)
        {
            return 0U;
        }
        const auto span = static_cast<float>(DSHOT_THROTTLE_MAX - DSHOT_THROTTLE_MIN);
        const auto value =
            static_cast<std::uint16_t>(DSHOT_THROTTLE_MIN + std::lroundf(motor * span));
        return value > DSHOT_THROTTLE_MAX ? DSHOT_THROTTLE_MAX : value;
    }

    /// @brief Builds the 16-bit DShot frame: 11 bits of value, the telemetry
    ///        request bit, then a 4-bit CRC (xor of the three value nibbles).
    /// @param value11 11-bit DShot value (dshotThrottle output)
    /// @param telemetry telemetry-request bit
    /// @return the frame, ready to clock out MSB first
    [[nodiscard]] inline std::uint16_t dshotFrame(std::uint16_t value11, bool telemetry)
    {
        const auto payload = static_cast<std::uint16_t>((value11 << 1U) | (telemetry ? 1U : 0U));
        const std::uint16_t crc = (payload ^ (payload >> 4U) ^ (payload >> 8U)) & 0x0FU;
        return static_cast<std::uint16_t>((payload << 4U) | crc);
    }

    /// @brief Expands one frame into per-bit high times, MSB first, into a
    ///        channel column of the burst buffer (stride DSHOT_MOTORS). The
    ///        two trailing slots stay at their zeroed value, parking the line
    ///        low after the last bit.
    /// @param frame 16-bit DShot frame
    /// @param dst first slot of the channel column
    /// @param stride distance between two slots of the same channel
    inline void dshotExpand(std::uint16_t frame, std::uint16_t *dst, std::size_t stride)
    {
        for (std::size_t bit = 0U; bit < DSHOT_BITS; ++bit)
        {
            const bool one = (frame & (0x8000U >> bit)) != 0U;
            dst[bit * stride] = one ? DSHOT_T1H : DSHOT_T0H;
        }
    }

    /// Drives four DShot600 ESC channels. TIM3 CH1-4 run PWM mode 1 on the
    /// motor pins; one DMA burst per timer update writes all four CCRs from
    /// the interleaved buffer, so one transfer clocks out a whole frame on
    /// the four lines at once. The buffer is a plain member (SRAM1/2, which
    /// is DMA-capable: it must never move to CCM).
    class MotorSinkDshot final : public AbsMotorSink
    {
      public:
        /// @brief Brings up the motor pins, TIM3 and the DMA stream. Leaves
        ///        the buffer at zero throttle so the ESCs arm on the stream
        ///        of valid zero frames the loop starts emitting.
        void init();

        /// @brief Encodes the four commands into the burst buffer and re-arms
        ///        the DMA stream. Never blocks. The frame of the motor a
        ///        requestTelemetry() named carries the telemetry bit; the
        ///        request is consumed whether or not the frame went out.
        /// @param frame actuator frame to output
        void push(const mark4::ActuatorFrame &frame) override;

        /// @brief Asks one ESC to answer on the telemetry wire: the next
        ///        push() sets the telemetry bit in that motor's frame, and
        ///        that one only. One request at a time, the last one wins.
        /// @param motor motor index, below DSHOT_MOTORS
        void requestTelemetry(std::size_t motor)
        {
            m_telemetryMotor = motor;
        }

        /// @return last frame pushed by the core, for the status log line
        [[nodiscard]] const mark4::ActuatorFrame &last() const
        {
            return m_last;
        }

      private:
        /// Interleaved by slot then channel: one TIM3 update bursts the four
        /// CCRs of one slot (buffer[slot * DSHOT_MOTORS + channel]).
        std::array<std::uint16_t, DSHOT_BUFFER> m_buffer{};
        mark4::ActuatorFrame m_last;  ///< last commands, for status logs
        std::uint32_t m_skipped = 0U; ///< frames dropped because the DMA was still busy
        std::size_t m_telemetryMotor = DSHOT_MOTORS; ///< pending request, DSHOT_MOTORS for none
    };
} // namespace mark4
