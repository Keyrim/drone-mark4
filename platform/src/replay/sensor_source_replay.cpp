#include "platform_replay/sensor_source_replay.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <ctime>

#include "flight_core/blackbox.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_S = 1000000U;
        constexpr std::uint64_t NS_PER_US = 1000U;

        /// @brief Sleeps for the given delay, best effort (interruptions and
        ///        scheduler latency are ignored: replay pacing is cosmetic).
        /// @param delayUs delay [us]
        void sleepUs(std::uint64_t delayUs)
        {
            timespec delay{};
            delay.tv_sec = static_cast<time_t>(delayUs / US_PER_S);
            delay.tv_nsec = static_cast<long>((delayUs % US_PER_S) * NS_PER_US);
            static_cast<void>(::nanosleep(&delay, nullptr));
        }
    } // namespace

    SensorSourceReplay::~SensorSourceReplay()
    {
        if (m_file != nullptr)
        {
            static_cast<void>(std::fclose(m_file));
            m_file = nullptr;
        }
    }

    bool SensorSourceReplay::init(const char *path)
    {
        if (m_file != nullptr)
        {
            static_cast<void>(std::fprintf(stderr, "SensorSourceReplay: already open\n"));
            return false;
        }

        m_file = std::fopen(path, "rb");
        if (m_file == nullptr)
        {
            static_cast<void>(std::fprintf(stderr,
                                           "SensorSourceReplay: fopen failed on '%s': %s\n",
                                           path,
                                           std::strerror(errno)));
            return false;
        }
        return true;
    }

    bool SensorSourceReplay::waitFrame(mark4::SensorFrame &frameOut)
    {
        if (m_file == nullptr)
        {
            return false;
        }

        std::array<std::uint8_t, BLACKBOX_RECORD_SIZE> bytes{};
        if (std::fread(bytes.data(), 1U, bytes.size(), m_file) != bytes.size())
        {
            return false; // end of file (a trailing partial record is dropped)
        }
        if (bytes[0] != BLACKBOX_VERSION)
        {
            static_cast<void>(std::fprintf(
                stderr, "SensorSourceReplay: unsupported record version %u\n", bytes[0]));
            return false;
        }

        BlackboxRecord record{};
        std::memcpy(&record, bytes.data(), sizeof(record));

        frameOut.timestampUs = record.timestampUs;
        /* The std::array members sit at odd offsets in the packed struct:
           reading them through it would bind a reference to a misaligned
           address, so they are copied straight out of the record bytes. */
        std::memcpy(frameOut.gyroRadS.data(),
                    bytes.data() + offsetof(BlackboxRecord, gyroRadS),
                    sizeof(frameOut.gyroRadS));
        std::memcpy(frameOut.accelMps2.data(),
                    bytes.data() + offsetof(BlackboxRecord, accelMps2),
                    sizeof(frameOut.accelMps2));
        frameOut.baroPa = record.baroPa;
        frameOut.rc.killSwitch = record.killSwitch != 0U;
        frameOut.rc.throttle = record.throttle;

        if (m_speedFactor > SPEED_MAX && m_hasPrevTimestamp &&
            record.timestampUs > m_prevTimestampUs)
        {
            const auto deltaUs = static_cast<float>(record.timestampUs - m_prevTimestampUs);
            sleepUs(static_cast<std::uint64_t>(deltaUs / m_speedFactor));
        }
        m_prevTimestampUs = record.timestampUs;
        m_hasPrevTimestamp = true;
        return true;
    }
} // namespace mark4
