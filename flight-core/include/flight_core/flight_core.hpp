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
    /// Phase of the flight state machine. The arm switch gates every phase
    /// where the core flies on its own: switching it off always cuts the
    /// motors and returns to IDLE, whatever was in progress.
    enum class FlightPhase : std::uint8_t
    {
        IDLE = 0U,      ///< motors stopped, waiting for the pilot
        MANUAL = 1U,    ///< stick flight: throttle commands the vertical velocity
        ARMED = 2U,     ///< armed for a throw: motors stopped, detector watched
        BALLISTIC = 3U, ///< throw detected, coasting until the spin-up instant
        RECOVERY = 4U,  ///< motors on, leveling from an arbitrary attitude
        HOVER = 5U,     ///< recovered: altitude hold until the pilot takes over
        CUTOFF = 6U,    ///< safety cutoff latched: motors stopped until rearm
    };

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

        /// Accel norm above which an impact cuts the motors. A hand throw
        /// peaks around 6 g; a crash spikes far beyond.
        static constexpr float CUTOFF_ACCEL_MPS2 = 8.0f * GRAVITY_MPS2;

        /// Gyro norm above which the motors are cut (sensor near saturation,
        /// nothing controlled is happening at such rates). Sits below the
        /// +/-2000 deg/s (34.9 rad/s) clip of the flight gyro, so one
        /// saturated axis alone always trips it, and well above real hand
        /// throw tumbling (measured at 10 rad/s or less).
        static constexpr float CUTOFF_GYRO_RADS = 30.0f;

        /// Cosine of the tilt beyond which the hover stack gives up (75 deg):
        /// it can only push the drone into the ground from there.
        static constexpr float CUTOFF_TILT_MIN_UP = 0.26f;

        /// The excessive tilt must last this long before cutting [us].
        static constexpr std::uint64_t CUTOFF_TILT_CONFIRM_US = 300000U;

        /// Motors are started this long before the predicted apex, so the
        /// props are at speed when it is reached. A calibration knob by
        /// nature: it covers the spool-up lag of the real motors.
        static constexpr std::uint64_t SPINUP_LEAD_US = 100000U;

        /// Cosine of the tilt under which the recovery is declared done and
        /// the altitude hold takes over (about 25 deg).
        static constexpr float RECOVERED_MIN_UP = 0.9f;

        /// A recovery still not level after this long is aborted [us]: the
        /// attitude estimate is likely wrong and the drone is pushing blind.
        static constexpr std::uint64_t RECOVERY_TIMEOUT_US = 2000000U;

        /// Floor of the recovery collective [0, 1]: torque authority scales
        /// with the motor commands, so some collective is always kept even
        /// upside down, where thrust itself is useless or harmful.
        static constexpr float RECOVERY_MIN_COLLECTIVE = 0.3f;

        /// Horizontal deceleration commanded per unit of estimated horizontal
        /// velocity in a post-throw hover [1/s]: the thrust tilts against the
        /// throw's momentum instead of letting the drone sail away. Brisk on
        /// purpose: the estimate is only trustworthy right after the throw,
        /// the momentum must be spent before the estimate is.
        static constexpr float BRAKE_GAIN = 1.0f;

        /// Tangent of the maximum braking tilt (about 20 deg): braking must
        /// stay a gentle lean, never a second acrobatic maneuver.
        static constexpr float BRAKE_TILT_MAX = 0.36f;

        /// Estimated horizontal speed under which the braking ends [m/s],
        /// permanently: it is a one-shot maneuver. The braking itself biases
        /// the attitude estimate (its specific force passes the Mahony gate)
        /// and the dead reckoning then rebuilds a phantom velocity out of
        /// that bias; chasing it would push the real drone backward forever.
        /// Level flight is the only drift-free long term attitude.
        static constexpr float BRAKE_DONE_MPS = 0.15f;

        /// Backstop on the braking duration after the recovery [us], in case
        /// the estimate never falls under BRAKE_DONE_MPS.
        static constexpr std::uint64_t BRAKE_WINDOW_US = 4000000U;

        /// @return current phase of the flight state machine
        [[nodiscard]] FlightPhase flightPhase() const
        {
            return m_phase;
        }

      private:
        void updateEstimators(const SensorFrame &sensors);
        void advancePhase(const SensorFrame &sensors);
        void runControl(const SensorFrame &sensors, ActuatorFrame &actuators);
        [[nodiscard]] bool cutoffTripped(const SensorFrame &sensors, bool withTilt = true);
        [[nodiscard]] float estimatedUpZ() const;
        [[nodiscard]] std::array<float, 3> brakeUpWorld() const;

        FlightPhase m_phase = FlightPhase::IDLE;
        std::uint32_t m_handledThrowCount = 0U;   ///< throws already acted upon
        std::uint64_t m_recoveryStartUs = 0U;     ///< entry instant of RECOVERY [us]
        std::uint64_t m_hoverStartUs = 0U;        ///< entry instant of HOVER [us]
        bool m_brakeDone = false;                 ///< braking spent for this flight
        std::uint64_t m_tiltExceededSinceUs = 0U; ///< start of the tilt streak, 0 = none
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
