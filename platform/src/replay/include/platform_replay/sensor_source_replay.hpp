#pragma once

/// @file
/// @brief Blackbox-fed sensor source for the replay variant.

#include <cstdint>
#include <cstdio>

#include "platform/sensor_source.hpp"

namespace mark4
{
    /// Replays the sensor frames recorded in a blackbox file, in open loop:
    /// the motor commands stored in the records are ignored. Pacing follows
    /// the recorded timestamps scaled by a speed factor, so a flight replays
    /// at its original tempo, slowed down, or as fast as it can be read.
    class SensorSourceReplay final : public AbsSensorSource
    {
      public:
        /// Speed factor meaning "no pacing": frames are produced as fast as
        /// they can be read from the file.
        static constexpr float SPEED_MAX = 0.0f;

        /// @param speedFactor 1.0f replays in real time, 0.1f ten times
        ///        slower, SPEED_MAX as fast as possible
        explicit SensorSourceReplay(float speedFactor)
            : m_speedFactor(speedFactor)
        {
        }

        ~SensorSourceReplay() override;

        SensorSourceReplay(const SensorSourceReplay &) = delete;
        SensorSourceReplay &operator=(const SensorSourceReplay &) = delete;
        SensorSourceReplay(SensorSourceReplay &&) = delete;
        SensorSourceReplay &operator=(SensorSourceReplay &&) = delete;

        /// @brief Opens the blackbox file. The reason is logged on failure.
        /// @param path blackbox file to replay
        /// @return true when the file is ready to be read
        bool init(const char *path);

        /// @brief Reads the next valid record, sleeping first so the frame
        ///        comes out on the recorded tempo scaled by the speed factor.
        ///        A damaged or unknown record costs only itself: the reader
        ///        resynchronizes on the next record sync marker.
        /// @param[out] frameOut frame decoded from the record
        /// @return FRAME when a record was decoded, EXHAUSTED at end of file
        FrameWait waitFrame(mark4::SensorFrame &frameOut) override;

      private:
        float m_speedFactor;                  ///< replay tempo scale, SPEED_MAX = unpaced
        std::FILE *m_file = nullptr;          ///< nullptr when closed
        bool m_hasPrevTimestamp = false;      ///< false until the first frame is out
        std::uint64_t m_prevTimestampUs = 0U; ///< pacing reference
    };
} // namespace mark4
