#pragma once

/// @file
/// @brief firmware composition root.

#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "platform_stm32/clock_stm32.hpp"
#include "platform_stm32/i2c_bus.hpp"
#include "platform_stm32/motor_sink_null.hpp"
#include "platform_stm32/mpu6050.hpp"
#include "platform_stm32/ms5611.hpp"
#include "platform_stm32/sensor_source_stm32.hpp"

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

        /// Frames between two heartbeat LED toggles: 1 Hz blink.
        static constexpr std::uint32_t FRAMES_PER_LED_TOGGLE =
            SensorSourceStm32::FRAME_RATE_HZ / 2U;

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
        mark4::FlightCore m_core;
    };
} // namespace mark4
