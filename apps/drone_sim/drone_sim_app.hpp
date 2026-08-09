#pragma once

/// @file
/// @brief drone_sim composition root.

#include <array>
#include <cstddef>
#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "platform_common/announce_publisher.hpp"
#include "platform_common/blackbox.hpp"
#include "platform_common/rc_tracker.hpp"
#include "platform_common/telemetry_publisher.hpp"
#include "platform_common/tuning_service.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/command_receiver_sim.hpp"
#include "platform_sim/log_sink_file.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/telemetry_sender_sim.hpp"
#include "platform_sim/udp_link.hpp"
#include "protocol/header.hpp"

namespace mark4
{
    /// Composition root: owns every service and the flight core as value members.
    /// Member declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no manual
    /// teardown. Built by main(), passed by reference: no singleton.
    class DroneSimApp
    {
      public:
        /// Delay without any sensor packet after which the run stops [ms].
        static constexpr std::uint32_t IDLE_TIMEOUT_MS = 2000U;

        /// Directory the blackbox file is created in, relative to the cwd.
        static constexpr const char *LOG_DIRECTORY = "logs";

        /// Size of the buffer holding the timestamped blackbox file path.
        static constexpr std::size_t LOG_PATH_SIZE = 64U;

        /// @param maxFrames number of frames to process before stopping
        /// @param simPort UDP port the sim link listens on
        /// @param telemetryPort UDP port telemetry is broadcast to (mirror
        ///        copies go to telemetryPort + 2)
        /// @param rcPort UDP port the command receiver binds, for the RC
        ///        stream the pilot keeps up
        explicit DroneSimApp(std::uint32_t maxFrames,
                             std::uint16_t simPort,
                             std::uint16_t telemetryPort,
                             std::uint16_t rcPort);

        /// @brief Initializes services in declaration order: binds the sim link,
        ///        opens the telemetry socket and the blackbox file. The first
        ///        failure is logged by the service and returns false immediately.
        /// @return true when every service is ready
        bool init();

        /// @brief Runs the waitFrame -> step -> push -> record loop until the
        ///        requested number of frames is reached or the sim link goes idle.
        /// @return number of steps executed
        std::uint32_t run();

        /// @return telemetry service, for post-run reporting
        [[nodiscard]] const mark4::TelemetrySenderSim &accessTelemetrySender() const
        {
            return m_telemetrySender;
        }

        /// @return motor sink, for post-run reporting
        [[nodiscard]] const mark4::MotorSinkSim &accessMotorSink() const
        {
            return m_motorSink;
        }

        /// @return clock service
        [[nodiscard]] mark4::AbsClock &accessClock()
        {
            return m_clock;
        }

        /// @return blackbox log sink, for post-run reporting
        [[nodiscard]] const mark4::LogSinkFile &accessLogSink() const
        {
            return m_logSink;
        }

        /// @return blackbox recorder, for post-run reporting
        [[nodiscard]] const mark4::Blackbox &accessBlackbox() const
        {
            return m_blackbox;
        }

      private:
        std::uint32_t m_maxFrames;     ///< frame budget for run()
        std::uint16_t m_simPort;       ///< sim link listen port
        std::uint16_t m_telemetryPort; ///< telemetry broadcast port
        std::uint16_t m_rcPort;        ///< command receiver listen port

        // Declaration order = construction order; dependencies are injected by
        // reference, so a service may only depend on those declared above it.
        mark4::ClockSim m_clock;
        mark4::UdpLink m_simLink;
        mark4::SensorSourceSim m_sensorSource;
        mark4::MotorSinkSim m_motorSink;
        mark4::TelemetrySenderSim m_telemetrySender;
        mark4::TelemetryPublisher m_telemetryPublisher{m_telemetrySender, StreamSource::DRONE_SIM};
        mark4::UdpLink m_rcLink;
        mark4::CommandReceiverSim m_commandReceiver{m_rcLink};
        mark4::RcTracker m_rcTracker{m_commandReceiver};
        mark4::TelemetrySenderSim m_announceSender;
        mark4::AnnouncePublisher m_announcePublisher{
            m_announceSender, StreamSource::DRONE_SIM, m_telemetryPort, m_rcPort};
        std::array<char, LOG_PATH_SIZE> m_logFilePath; ///< one file per run, outlives m_logSink
        mark4::LogSinkFile m_logSink;
        mark4::FlightCore m_core;
        mark4::TuningService m_tuningService{m_core, m_telemetrySender};
        mark4::Blackbox m_blackbox;
    };
} // namespace mark4
