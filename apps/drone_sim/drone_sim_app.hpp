#pragma once

/// @file
/// @brief drone_sim composition root.

#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/telemetry_sender_sim.hpp"

namespace mark4
{
    /// Composition root: owns every service and the flight core as value members.
    /// Member declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no manual
    /// teardown. Built by main(), passed by reference: no singleton.
    class DroneSimApp
    {
      public:
        /// Sensor frame period [us] (1 kHz, typical gyro rate).
        static constexpr std::uint64_t FRAME_PERIOD_US = 1000U;

        /// @param iterations number of frames to run before stopping
        explicit DroneSimApp(std::uint32_t iterations);

        /// @brief Initializes services in declaration order.
        ///
        /// Current services cannot fail; networked ones will check and log here,
        /// following the rule: first failure returns false immediately.
        /// @return true when every service is ready
        bool init();

        /// @brief Runs the waitFrame -> step -> push loop until the source is
        ///        exhausted.
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

      private:
        void sendTelemetry(const mark4::SensorFrame &frame, const mark4::ActuatorFrame &actuators);

        // Declaration order = construction order; dependencies are injected by
        // reference, so a service may only depend on those declared above it.
        mark4::ClockSim m_clock;
        mark4::SensorSourceSim m_sensorSource;
        mark4::MotorSinkSim m_motorSink;
        mark4::TelemetrySenderSim m_telemetrySender;
        mark4::FlightCore m_core;
    };
} // namespace mark4
