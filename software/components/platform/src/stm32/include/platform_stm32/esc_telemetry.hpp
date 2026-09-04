#pragma once

/// @file
/// @brief ESC telemetry over the shared TLM wire: the KISS frame codec and
///        the sequencer that asks one ESC at a time through the DShot
///        telemetry bit. Register-free on purpose, so a desktop test pins
///        the decoding and the sequencing without a board; the USART behind
///        it lives in esc_uart.hpp.

#include <array>
#include <cstddef>
#include <cstdint>

#include "telemetry/registry.hpp"

namespace mark4
{
    /// ESCs on the wire: the four channels of the 4-in-1.
    constexpr std::size_t ESC_COUNT = 4U;

    /// The KISS telemetry frame: 9 data bytes and a CRC-8.
    constexpr std::size_t KISS_FRAME_SIZE = 10U;

    /// Byte rate every KISS-compatible ESC (BLHeli32, AM32) answers at.
    constexpr std::uint32_t ESC_TELEMETRY_BAUD_RATE = 115200U;

    /// One decoded frame, already in the units the measures carry.
    struct EscTelemetrySample
    {
        float temperatureC = 0.0f;   ///< ESC temperature [degC]
        float voltageV = 0.0f;       ///< pack voltage as the ESC sees it [V]
        float currentA = 0.0f;       ///< phase current [A]
        float consumptionMah = 0.0f; ///< charge drawn since the ESC powered up [mAh]
        float erpm = 0.0f;           ///< electrical rpm (mechanical rpm x pole pairs)
    };

    /// @brief CRC-8 of the KISS telemetry frame: polynomial 0x07, no init,
    ///        no reflection, no final xor.
    /// @param data bytes covered
    /// @param size byte count
    /// @return the checksum
    [[nodiscard]] inline std::uint8_t kissCrc8(const std::uint8_t *data, std::size_t size)
    {
        constexpr std::uint8_t POLYNOMIAL = 0x07U;
        std::uint8_t crc = 0U;
        for (std::size_t index = 0U; index < size; ++index)
        {
            crc ^= data[index];
            for (std::size_t bit = 0U; bit < 8U; ++bit)
            {
                crc = static_cast<std::uint8_t>((crc & 0x80U) != 0U ? (crc << 1U) ^ POLYNOMIAL
                                                                    : (crc << 1U));
            }
        }
        return crc;
    }

    /// @brief Decodes one KISS frame: temperature in whole degrees, voltage
    ///        and current in hundredths, consumption in mAh, eRPM in
    ///        hundreds, every field big-endian, the last byte the CRC of
    ///        the nine before it.
    /// @param frame KISS_FRAME_SIZE bytes
    /// @param[out] sampleOut decoded sample, untouched on a CRC mismatch
    /// @return true when the CRC matched
    [[nodiscard]] inline bool decodeKissFrame(const std::uint8_t *frame,
                                              EscTelemetrySample &sampleOut)
    {
        if (kissCrc8(frame, KISS_FRAME_SIZE - 1U) != frame[KISS_FRAME_SIZE - 1U])
        {
            return false;
        }
        const auto u16 = [frame](std::size_t at) {
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(frame[at]) << 8U) |
                                              frame[at + 1U]);
        };
        constexpr float HUNDREDTH = 0.01f;
        constexpr float HUNDRED = 100.0f;
        sampleOut.temperatureC = static_cast<float>(frame[0]);
        sampleOut.voltageV = static_cast<float>(u16(1U)) * HUNDREDTH;
        sampleOut.currentA = static_cast<float>(u16(3U)) * HUNDREDTH;
        sampleOut.consumptionMah = static_cast<float>(u16(5U));
        sampleOut.erpm = static_cast<float>(u16(7U)) * HUNDRED;
        return true;
    }

    /// Follows the four ESCs of the 4-in-1 on their one shared TLM wire.
    /// Only the ESC whose DShot frame carried the telemetry bit answers, so
    /// the sequencer keeps a single request outstanding: the bytes that
    /// follow belong to that ESC, a complete frame with a good CRC records
    /// its sample and the next ESC gets the bit, a request nobody answered
    /// within REQUEST_TIMEOUT_US is counted and skipped. Bytes arriving with
    /// no request outstanding are counted as stray and dropped, which is
    /// also how the stream resynchronizes after a corrupted frame.
    ///
    /// The composition drives it once per output frame: every byte the
    /// USART received goes through feed(), then nextRequest() says which
    /// motor the sink must flag in the frame it is about to push.
    class EscTelemetry
    {
      public:
        /// Time an ESC gets to answer before its turn is skipped [us]: ten
        /// output frames, far above the millisecond a frame takes on the
        /// wire, so an ESC that is simply absent costs nothing but a
        /// counter every so often.
        static constexpr std::uint64_t REQUEST_TIMEOUT_US = 20000U;

        /// Age past which an ESC that was reporting counts as silent [us].
        static constexpr std::uint64_t SILENCE_US = 1000000U;

        /// @brief Takes one byte received on the TLM wire.
        /// @param byte received byte
        /// @param nowUs instant of the read, stamps a completed frame [us]
        void feed(std::uint8_t byte, std::uint64_t nowUs)
        {
            if (!m_pending)
            {
                ++m_strayBytes;
                return;
            }
            m_buffer[m_fill] = byte;
            ++m_fill;
            if (m_fill < KISS_FRAME_SIZE)
            {
                return;
            }
            EscRecord &record = m_records[m_motor];
            if (decodeKissFrame(m_buffer.data(), record.sample))
            {
                record.lastFrameUs = nowUs;
                record.seen = true;
                ++m_frames;
            }
            else
            {
                ++m_crcErrors;
            }
            m_pending = false;
            advance();
        }

        /// @brief Decides the telemetry request of the frame about to go
        ///        out: the next ESC in turn when nothing is outstanding, or
        ///        when the outstanding request timed out.
        /// @param nowUs instant of the frame [us]
        /// @param[out] motorOut motor whose frame must carry the telemetry
        ///        bit, valid only when returning true
        /// @return true when a request is issued this frame
        bool nextRequest(std::uint64_t nowUs, std::size_t &motorOut)
        {
            if (m_pending)
            {
                if (nowUs - m_requestUs < REQUEST_TIMEOUT_US)
                {
                    return false;
                }
                ++m_timeouts;
                advance();
            }
            m_pending = true;
            m_requestUs = nowUs;
            m_fill = 0U;
            motorOut = m_motor;
            return true;
        }

        /// @param motor ESC index, below ESC_COUNT
        /// @return the last sample decoded for that ESC, zeros until one came
        [[nodiscard]] const EscTelemetrySample &sample(std::size_t motor) const
        {
            return m_records[motor].sample;
        }

        /// @param motor ESC index, below ESC_COUNT
        /// @param nowUs current instant [us]
        /// @return true when that ESC delivered a frame within SILENCE_US
        [[nodiscard]] bool online(std::size_t motor, std::uint64_t nowUs) const
        {
            const EscRecord &record = m_records[motor];
            return record.seen && nowUs - record.lastFrameUs < SILENCE_US;
        }

        /// @return frames decoded with a good CRC, all ESCs together
        [[nodiscard]] std::uint32_t frames() const
        {
            return m_frames;
        }

        /// @return complete frames rejected on their CRC
        [[nodiscard]] std::uint32_t crcErrors() const
        {
            return m_crcErrors;
        }

        /// @return requests no frame answered in time
        [[nodiscard]] std::uint32_t timeouts() const
        {
            return m_timeouts;
        }

        /// @return bytes received while no request was outstanding
        [[nodiscard]] std::uint32_t strayBytes() const
        {
            return m_strayBytes;
        }

      private:
        /// What is known of one ESC.
        struct EscRecord
        {
            EscTelemetrySample sample;      ///< last decoded frame
            std::uint64_t lastFrameUs = 0U; ///< instant of that frame [us]
            bool seen = false;              ///< at least one frame decoded
        };

        /// @brief Moves the turn to the next ESC, round-robin.
        void advance()
        {
            m_motor = (m_motor + 1U) % ESC_COUNT;
        }

        /// @param context the EscTelemetry the entry was built with
        /// @return frames decoded, as a float measure
        static float ReadFrames(const void *context)
        {
            return static_cast<float>(static_cast<const EscTelemetry *>(context)->m_frames);
        }

        /// @param context the EscTelemetry the entry was built with
        /// @return CRC rejections, as a float measure
        static float ReadCrcErrors(const void *context)
        {
            return static_cast<float>(static_cast<const EscTelemetry *>(context)->m_crcErrors);
        }

        /// @param context the EscTelemetry the entry was built with
        /// @return timed out requests, as a float measure
        static float ReadTimeouts(const void *context)
        {
            return static_cast<float>(static_cast<const EscTelemetry *>(context)->m_timeouts);
        }

        std::array<EscRecord, ESC_COUNT> m_records{};         ///< one per ESC
        std::array<std::uint8_t, KISS_FRAME_SIZE> m_buffer{}; ///< frame being received
        std::size_t m_fill = 0U;                              ///< bytes in m_buffer
        std::size_t m_motor = 0U;                             ///< ESC whose turn it is
        bool m_pending = false;                               ///< a request is outstanding
        std::uint64_t m_requestUs = 0U;                       ///< instant of that request [us]
        std::uint32_t m_frames = 0U;                          ///< good frames
        std::uint32_t m_crcErrors = 0U;                       ///< rejected frames
        std::uint32_t m_timeouts = 0U;                        ///< unanswered requests
        std::uint32_t m_strayBytes = 0U;                      ///< bytes outside a request

        // Measures: the five fields of every ESC, then the wire health. The
        // consumption is what the ESC integrated since it powered up, so it
        // restarts at zero with the pack.
        TelemetryEntry m_temperature0{
            "esc/0/temperature", TelemetryUnit::CELSIUS, m_records[0].sample.temperatureC};
        TelemetryEntry m_voltage0{"esc/0/voltage", TelemetryUnit::V, m_records[0].sample.voltageV};
        TelemetryEntry m_current0{"esc/0/current", TelemetryUnit::A, m_records[0].sample.currentA};
        TelemetryEntry m_consumption0{
            "esc/0/consumption", TelemetryUnit::MAH, m_records[0].sample.consumptionMah};
        TelemetryEntry m_erpm0{"esc/0/erpm", TelemetryUnit::RPM, m_records[0].sample.erpm};
        TelemetryEntry m_temperature1{
            "esc/1/temperature", TelemetryUnit::CELSIUS, m_records[1].sample.temperatureC};
        TelemetryEntry m_voltage1{"esc/1/voltage", TelemetryUnit::V, m_records[1].sample.voltageV};
        TelemetryEntry m_current1{"esc/1/current", TelemetryUnit::A, m_records[1].sample.currentA};
        TelemetryEntry m_consumption1{
            "esc/1/consumption", TelemetryUnit::MAH, m_records[1].sample.consumptionMah};
        TelemetryEntry m_erpm1{"esc/1/erpm", TelemetryUnit::RPM, m_records[1].sample.erpm};
        TelemetryEntry m_temperature2{
            "esc/2/temperature", TelemetryUnit::CELSIUS, m_records[2].sample.temperatureC};
        TelemetryEntry m_voltage2{"esc/2/voltage", TelemetryUnit::V, m_records[2].sample.voltageV};
        TelemetryEntry m_current2{"esc/2/current", TelemetryUnit::A, m_records[2].sample.currentA};
        TelemetryEntry m_consumption2{
            "esc/2/consumption", TelemetryUnit::MAH, m_records[2].sample.consumptionMah};
        TelemetryEntry m_erpm2{"esc/2/erpm", TelemetryUnit::RPM, m_records[2].sample.erpm};
        TelemetryEntry m_temperature3{
            "esc/3/temperature", TelemetryUnit::CELSIUS, m_records[3].sample.temperatureC};
        TelemetryEntry m_voltage3{"esc/3/voltage", TelemetryUnit::V, m_records[3].sample.voltageV};
        TelemetryEntry m_current3{"esc/3/current", TelemetryUnit::A, m_records[3].sample.currentA};
        TelemetryEntry m_consumption3{
            "esc/3/consumption", TelemetryUnit::MAH, m_records[3].sample.consumptionMah};
        TelemetryEntry m_erpm3{"esc/3/erpm", TelemetryUnit::RPM, m_records[3].sample.erpm};
        TelemetryEntry m_framesEntry{"esc/frames", TelemetryUnit::COUNT, this, &ReadFrames};
        TelemetryEntry m_crcErrorsEntry{
            "esc/crc_errors", TelemetryUnit::COUNT, this, &ReadCrcErrors};
        TelemetryEntry m_timeoutsEntry{"esc/timeouts", TelemetryUnit::COUNT, this, &ReadTimeouts};
    };
} // namespace mark4
