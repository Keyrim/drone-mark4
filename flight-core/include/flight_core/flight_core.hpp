#pragma once

/// @file
/// @brief Flight core entry point.

#include <array>
#include <cstdint>

#include "flight_core/attitude_controller.hpp"
#include "flight_core/attitude_estimator.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/throw_detector.hpp"
#include "flight_core/types.hpp"
#include "flight_core/vertical_controller.hpp"
#include "flight_core/vertical_estimator.hpp"

namespace mark4
{
    /// Synchronous, single-threaded flight core, paced by data arrival
    /// (never by time). Pure: no allocation, no waiting, no clock access.
    class FlightCore
    {
      public:
        /// @brief Runs one control step. The kill switch is honored first.
        /// @param sensors latest sensor frame
        /// @param[out] actuators motor commands computed for this step
        void step(const SensorFrame &sensors, ActuatorFrame &actuators);

        /// @return number of steps executed since construction
        [[nodiscard]] std::uint32_t stepCount() const
        {
            return m_stepCount;
        }

        /// @return estimated body-to-world attitude
        [[nodiscard]] const Quaternion &attitude() const
        {
            return m_attitudeEstimator.attitude();
        }

        /// @return estimated constant gyro bias [rad/s]
        [[nodiscard]] std::array<float, 3> gyroBiasRadS() const
        {
            return m_attitudeEstimator.gyroBiasRadS();
        }

        /// @return estimated altitude above the startup reference [m]
        [[nodiscard]] float altitudeM() const
        {
            return m_verticalEstimator.altitudeM();
        }

        /// @return estimated vertical velocity, positive up [m/s]
        [[nodiscard]] float verticalVelocityMps() const
        {
            return m_verticalEstimator.verticalVelocityMps();
        }

        /// @return throw detector, source of the apex prediction
        [[nodiscard]] const ThrowDetector &throwDetector() const
        {
            return m_throwDetector;
        }

        /// Throttle below which the drone stays disarmed: motors stopped and
        /// control integrators held at zero.
        static constexpr float ARM_THROTTLE = 0.05f;

        /// Vertical velocity setpoint at full stick deflection [m/s]; mid
        /// stick holds the altitude.
        static constexpr float STICK_VZ_RANGE_MPS = 2.0f;

        /// Control steps longer than this are gaps: the loop outputs are
        /// recomputed but the integrators do not integrate the hole.
        static constexpr float MAX_CONTROL_STEP_S = 0.05f;

      private:
        void updateEstimators(const SensorFrame &sensors);
        void runControl(const SensorFrame &sensors, ActuatorFrame &actuators);

        std::uint32_t m_stepCount = 0U;
        AttitudeEstimator m_attitudeEstimator;
        VerticalEstimator m_verticalEstimator;
        ThrowDetector m_throwDetector;
        AttitudeController m_attitudeController;
        RateController m_rateController;
        VerticalController m_verticalController;
        std::uint64_t m_prevControlTimestampUs = 0U; ///< control step reference [us]
    };
} // namespace mark4
