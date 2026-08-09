#pragma once

/// @file
/// @brief firmware composition root.

#include <cstdint>

#include "flight_core/blackbox.hpp"
#include "flight_core/flight_core.hpp"
#include "platform_common/telemetry_publisher.hpp"
#include "platform_stm32/clock_stm32.hpp"
#include "platform_stm32/command_receiver_stm32.hpp"
#include "platform_stm32/i2c_bus.hpp"
#include "platform_stm32/log_sink_uart.hpp"
#include "platform_stm32/motor_sink_null.hpp"
#include "platform_stm32/mpu6050.hpp"
#include "platform_stm32/ms5611.hpp"
#include "platform_stm32/sensor_source_stm32.hpp"
#include "platform_stm32/telemetry_sender_stm32.hpp"
#include "protocol/header.hpp"

namespace mark4
{
    /// Composition root: owns every service and the flight core as value
    /// members. Member declaration order IS the construction order, and a
    /// service may only depend on those declared above it. Built by
    /// main(), passed by reference: no singleton.
    class FirmwareApp
    {
      public:
        /// Frames between two status lines over RTT: one per second.
        static constexpr std::uint32_t FRAMES_PER_STATUS = SensorSourceStm32::FRAME_RATE_HZ;

        /// Fail-safe: silence on the RC uplink longer than this reverts
        /// to the safe state (kill engaged, disarmed). Five missed
        /// packets at the 10 Hz the sender streams at.
        static constexpr std::uint64_t RC_TIMEOUT_US = 500000U;

        FirmwareApp() = default;

        /// @brief Initializes the board (clock tree, RTT console, LEDs)
        ///        then the services in declaration order. The first
        ///        failure is logged over RTT and returns false.
        /// @return true when the loop is ready to run
        bool init();

        /// @brief Runs the waitFrame -> step -> push loop forever, with a
        ///        heartbeat LED and a one-line status over RTT every
        ///        second.
        [[noreturn]] void run();

      private:
        // Declaration order = construction order; dependencies are
        // injected by reference, so a service may only depend on those
        // declared above it.
        mark4::ClockStm32 m_clock;
        mark4::I2cBus m_bus;
        mark4::Mpu6050 m_imu{m_bus};
        mark4::Ms5611 m_baro{m_bus};
        mark4::SensorSourceStm32 m_sensorSource{m_imu, m_baro, m_clock};
        mark4::MotorSinkNull m_motorSink;
        mark4::TelemetrySenderStm32 m_telemetrySender;
        mark4::TelemetryPublisher m_telemetryPublisher{m_telemetrySender, StreamSource::FIRMWARE};
        mark4::CommandReceiverStm32 m_commandReceiver;
        mark4::LogSinkUart m_logSink{m_telemetrySender};
        mark4::Blackbox m_blackbox{m_logSink};
        mark4::FlightCore m_core;
    };
} // namespace mark4
