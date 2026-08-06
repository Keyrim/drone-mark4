#pragma once

/// @file
/// @brief drone_sim composition root.

#include <array>
#include <cstddef>
#include <cstdint>

#include "flight_core/blackbox.hpp"
#include "flight_core/flight_core.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/log_sink_file.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/telemetry_sender_sim.hpp"
#include "platform_sim/udp_link.hpp"

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
        explicit DroneSimApp(std::uint32_t maxFrames,
                             std::uint16_t simPort,
                             std::uint16_t telemetryPort);

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
        void sendTelemetry(const mark4::SensorFrame &frame, const mark4::ActuatorFrame &actuators);

        std::uint32_t m_maxFrames;     ///< frame budget for run()
        std::uint16_t m_simPort;       ///< sim link listen port
        std::uint16_t m_telemetryPort; ///< telemetry broadcast port

        // Declaration order = construction order; dependencies are injected by
        // reference, so a service may only depend on those declared above it.
        mark4::ClockSim m_clock;
        mark4::UdpLink m_simLink;
        mark4::SensorSourceSim m_sensorSource;
        mark4::MotorSinkSim m_motorSink;
        mark4::TelemetrySenderSim m_telemetrySender;
        std::array<char, LOG_PATH_SIZE> m_logFilePath; ///< one file per run, outlives m_logSink
        mark4::LogSinkFile m_logSink;
        mark4::FlightCore m_core;
        mark4::Blackbox m_blackbox;
    };
} // namespace mark4
