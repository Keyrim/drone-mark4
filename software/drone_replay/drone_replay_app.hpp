#pragma once

/// @file
/// @brief drone_replay composition root.

#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "platform_common/announce_publisher.hpp"
#include "platform_common/telemetry_publisher.hpp"
#include "platform_replay/sensor_source_replay.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/telemetry_sender_sim.hpp"
#include "protocol/header.hpp"
#include "protocol/ports.hpp"

namespace mark4
{
    /// Composition root: replays a blackbox file through the flight core in
    /// open loop (the recorded motor outputs are ignored) and re-broadcasts
    /// telemetry, so the usual listeners can watch a past run. Same member
    /// ordering contract as DroneSimApp: declaration order = construction and
    /// initialization order, destruction is the automatic reverse.
    class DroneReplayApp
    {
      public:
        /// @param path blackbox file to replay; not copied, must outlive the app
        /// @param speedFactor replay tempo, SensorSourceReplay::SPEED_MAX = unpaced
        /// @param sessionId identity of this process start, announced so the
        ///        ground side reads a restart as a restart
        /// @param announcePort port the process announce is broadcast to
        DroneReplayApp(const char *path,
                       float speedFactor,
                       std::uint32_t sessionId,
                       std::uint16_t announcePort);

        /// @brief Initializes services in declaration order: opens the blackbox
        ///        file and the telemetry socket. The first failure is logged by
        ///        the service and returns false immediately.
        /// @return true when every service is ready
        bool init();

        /// @brief Runs the waitFrame -> step -> telemetry loop until the file
        ///        is exhausted.
        /// @return number of steps executed
        std::uint32_t run();

        /// @return telemetry service, for post-run reporting
        [[nodiscard]] const mark4::TelemetrySenderSim &accessTelemetrySender() const
        {
            return m_telemetrySender;
        }

        /// @return flight core, for post-run reporting
        [[nodiscard]] const mark4::FlightCore &accessFlightCore() const
        {
            return m_core;
        }

      private:
        const char *m_logPath;        ///< blackbox file to replay, not owned
        std::uint32_t m_sessionId;    ///< identity of this process start
        std::uint16_t m_announcePort; ///< port the announce is broadcast to

        // Declaration order = construction order; dependencies are injected by
        // reference, so a service may only depend on those declared above it.
        mark4::ClockSim m_clock;
        mark4::SensorSourceReplay m_sensorSource;
        mark4::TelemetrySenderSim m_telemetrySender;
        mark4::TelemetryPublisher m_telemetryPublisher{m_telemetrySender,
                                                       StreamSource::DRONE_REPLAY};
        /// Announces on its own socket: the telemetry sender is bound to the
        /// telemetry port and announces travel to a port of their own.
        mark4::TelemetrySenderSim m_announceSender;
        /// No command port: nothing steers a replay, so 0 says "reaches me
        /// nowhere" rather than pointing the ground side at a dead socket.
        mark4::AnnouncePublisher m_announcePublisher{
            m_announceSender, StreamSource::DRONE_REPLAY, m_sessionId, TELEMETRY_PORT, 0U};
        mark4::FlightCore m_core;
    };
} // namespace mark4
