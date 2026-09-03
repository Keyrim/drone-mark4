#pragma once

/// @file
/// @brief Integrity tracking of one simulated run: a hash of the trajectory
///        it produced and the health of the link that carried it.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flight_core/types.hpp"
#include "platform_common/envelope_io.hpp"
#include "protocol/envelope.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    /// Boils one simulated run down to a single 64 bit number, so two runs
    /// given the same scenario can be compared instead of eyeballed, and
    /// watches the link that carried them so a number produced over a
    /// degraded link is never mistaken for a verdict.
    ///
    /// The hash is taken over time RELATIVE to the reset that opened the run.
    /// The absolute instant a run starts at says how long the process had
    /// been up, which is not a property of the run: two identical runs played
    /// at different absolute times must hash equal, and that is precisely
    /// what makes the number worth comparing.
    ///
    /// The RC state is deliberately left out of the hash. It arrives
    /// out-of-band, paced by the host wall clock rather than by the plant
    /// tick grid, so which frame carries a given arm state is not a property
    /// of the run either; hashing it would make every comparison flaky.
    class SimRunTracker
    {
      public:
        /// Simulated time hashed after the start of a run when the scenario
        /// asks for no window of its own [us].
        static constexpr std::uint32_t DEFAULT_HASH_WINDOW_US = 10000000U;

        /// FNV-1a 64 bit offset basis.
        static constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;

        /// FNV-1a 64 bit prime.
        static constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

        /// Bytes fed to the hash per stepped frame: relative timestamp (8)
        /// + gyro (12) + accel (12) + baro (4) + motors (16).
        static constexpr std::size_t HASHED_BYTES_PER_FRAME = 52U;

        /// Frames between two stats messages when nothing changed: a consumer
        /// that joined late still learns where the run stands, without the
        /// stream becoming a second telemetry.
        static constexpr std::uint32_t PUBLISH_PERIOD_FRAMES = 50U;

        static_assert(std::endian::native == std::endian::little,
                      "the hash is defined over little-endian bytes");

        /// @param transport transport the run stats are broadcast on
        explicit SimRunTracker(Transport &transport)
            : m_transport(transport)
        {
        }

        /// @brief Opens a run: everything measured from here on belongs to it.
        /// @param runId reset counter of the run being measured
        /// @param startUs simulated time of the first frame of the run [us]
        /// @param hashWindowUs simulated time to hash, 0 for the default
        void beginRun(std::uint32_t runId, std::uint64_t startUs, std::uint32_t hashWindowUs)
        {
            m_runId = runId;
            m_runStartUs = startUs;
            m_hashWindowUs = hashWindowUs == 0U ? DEFAULT_HASH_WINDOW_US : hashWindowUs;
            m_hash = FNV_OFFSET_BASIS;
            m_hashedFrames = 0U;
            m_sealed = false;
            m_degraded = false;
            m_linkSeen = false;
            m_running = true;
        }

        /// @brief Folds one stepped frame into the hash, and seals the hash
        ///        once the window has elapsed. A frame outside an open run,
        ///        or one arriving after the seal, changes nothing.
        /// @param frame sensor frame of the step
        /// @param actuators actuator outputs of the step
        void update(const SensorFrame &frame, const ActuatorFrame &actuators)
        {
            if (!m_running || m_sealed || frame.timestampUs < m_runStartUs)
            {
                return;
            }
            const std::uint64_t relativeUs = frame.timestampUs - m_runStartUs;
            if (relativeUs >= m_hashWindowUs)
            {
                m_sealed = true;
                return;
            }

            std::array<std::uint8_t, HASHED_BYTES_PER_FRAME> bytes{};
            std::size_t at = 0U;
            std::memcpy(bytes.data() + at, &relativeUs, sizeof(relativeUs));
            at += sizeof(relativeUs);
            std::memcpy(bytes.data() + at, frame.gyroRadS.data(), sizeof(frame.gyroRadS));
            at += sizeof(frame.gyroRadS);
            std::memcpy(bytes.data() + at, frame.accelMps2.data(), sizeof(frame.accelMps2));
            at += sizeof(frame.accelMps2);
            std::memcpy(bytes.data() + at, &frame.baroPa, sizeof(frame.baroPa));
            at += sizeof(frame.baroPa);
            std::memcpy(bytes.data() + at, actuators.motor.data(), sizeof(actuators.motor));

            for (const std::uint8_t byte : bytes)
            {
                m_hash ^= static_cast<std::uint64_t>(byte);
                m_hash *= FNV_PRIME;
            }
            ++m_hashedFrames;
        }

        /// @brief Takes the running link counters. A rise of the lockstep
        ///        timeout counter inside a run marks the run degraded, and
        ///        the mark never clears: a trajectory that lost ticks is not
        ///        the trajectory the scenario asked for, whatever happens
        ///        afterwards.
        /// @param lockstepTimeouts cumulative lockstep timeouts of the plant
        /// @param duplicateFrames sensor resends rejected since startup
        void noteLink(std::uint32_t lockstepTimeouts, std::uint32_t duplicateFrames)
        {
            if (m_linkSeen && lockstepTimeouts > m_lockstepTimeouts)
            {
                m_degraded = true;
            }
            m_lockstepTimeouts = lockstepTimeouts;
            m_duplicateFrames = duplicateFrames;
            m_linkSeen = true;
        }

        /// @brief Broadcasts where the run stands, when it is worth saying:
        ///        on every change of the run or of its flags, and otherwise
        ///        once per PUBLISH_PERIOD_FRAMES. Called once per stepped
        ///        frame; the pacing is this class's business, not the
        ///        caller's.
        void publish()
        {
            ++m_framesSincePublish;
            const bool changed = !m_everPublished || m_sealed != m_publishedSealed ||
                                 m_degraded != m_publishedDegraded || m_runId != m_publishedRunId;
            if (!changed && m_framesSincePublish < PUBLISH_PERIOD_FRAMES)
            {
                return;
            }
            m_framesSincePublish = 0U;
            m_publishedSealed = m_sealed;
            m_publishedDegraded = m_degraded;
            m_publishedRunId = m_runId;
            m_everPublished = true;

            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_sim_run_stats_tag;
            mark4_SimRunStats &stats = envelope.body.sim_run_stats;
            stats.run_id = m_runId;
            stats.final = m_sealed;
            stats.degraded = m_degraded;
            stats.run_start_us = m_runStartUs;
            stats.run_hash = m_hash;
            stats.duplicate_frames = m_duplicateFrames;
            stats.lockstep_timeouts = m_lockstepTimeouts;
            static_cast<void>(sendEnvelope(m_transport, BROADCAST_NODE, envelope));
        }

        /// @return reset counter of the run being measured
        [[nodiscard]] std::uint32_t runId() const
        {
            return m_runId;
        }

        /// @return simulated time the run started at [us]
        [[nodiscard]] std::uint64_t runStartUs() const
        {
            return m_runStartUs;
        }

        /// @return hash of the run so far, final once sealed() is true
        [[nodiscard]] std::uint64_t hash() const
        {
            return m_hash;
        }

        /// @return frames folded into the hash since the run opened
        [[nodiscard]] std::uint32_t hashedFrames() const
        {
            return m_hashedFrames;
        }

        /// @return true once the hash window elapsed and the hash is final
        [[nodiscard]] bool sealed() const
        {
            return m_sealed;
        }

        /// @return true when the link lost a tick during this run
        [[nodiscard]] bool degraded() const
        {
            return m_degraded;
        }

        /// @return lockstep timeouts last reported by the plant
        [[nodiscard]] std::uint32_t lockstepTimeouts() const
        {
            return m_lockstepTimeouts;
        }

        /// @return sensor resends rejected by the flight process
        [[nodiscard]] std::uint32_t duplicateFrames() const
        {
            return m_duplicateFrames;
        }

      private:
        Transport &m_transport;                                ///< output, not owned
        std::uint64_t m_hash = FNV_OFFSET_BASIS;               ///< running trajectory hash
        std::uint64_t m_runStartUs = 0U;                       ///< simulated start of the run [us]
        std::uint32_t m_hashWindowUs = DEFAULT_HASH_WINDOW_US; ///< hashed window [us]
        std::uint32_t m_hashedFrames = 0U;                     ///< frames folded in
        std::uint32_t m_lockstepTimeouts = 0U;                 ///< last plant timeout count
        std::uint32_t m_duplicateFrames = 0U;                  ///< last resend count
        std::uint32_t m_framesSincePublish = 0U;               ///< frames since the last message
        std::uint32_t m_runId = 0U;                            ///< reset counter of the run
        std::uint32_t m_publishedRunId = 0U;                   ///< run id of the last message
        bool m_publishedSealed = false;                        ///< seal flag of the last message
        bool m_publishedDegraded = false; ///< degraded flag of the last message
        bool m_everPublished = false;     ///< a message has gone out
        bool m_running = false;           ///< a run is open
        bool m_sealed = false;            ///< the window elapsed
        bool m_degraded = false;          ///< the link lost a tick
        bool m_linkSeen = false;          ///< a link report was taken
    };
} // namespace mark4
