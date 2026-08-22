#pragma once

/// @file
/// @brief Quaternion attitude estimator (Mahony filter).

#include <array>
#include <cstdint>

#include "flight_core/types.hpp"

namespace mark4
{
    /// Estimates the body attitude by integrating the gyro and correcting the
    /// result toward the gravity direction measured by the accelerometer
    /// (Mahony filter). The PI feedback on the direction error also estimates
    /// the constant gyro bias.
    ///
    /// The correction is gated on the accelerometer norm: away from 1 g
    /// (free fall, throw thrust, impacts) the specific force says nothing
    /// about gravity and the estimator falls back to pure gyro integration.
    /// Yaw is unobservable from gravity alone and drifts slowly; that is
    /// acceptable for attitude recovery, where only the up direction matters.
    class AttitudeEstimator
    {
      public:
        /// Default proportional gain: converges from a large error in a few
        /// seconds on the pad without yanking the attitude in flight.
        static constexpr float DEFAULT_KP = 2.0f;

        /// Default integral gain: slow on purpose, the bias is near constant.
        static constexpr float DEFAULT_KI = 0.1f;

        /// Half-width of the accel norm window around 1 g inside which the
        /// gravity correction is trusted.
        static constexpr float ACCEL_GATE_MPS2 = 1.0f;

        /// Gyro norm above which the gravity correction is not trusted
        /// either [rad/s]: a body rotating this fast is being maneuvered (a
        /// hand starting a throw, a tumble) and the specific force direction
        /// no longer points along gravity even when its norm stays near 1 g.
        /// Hover jitter and a held hand's sway stay well under this.
        static constexpr float GYRO_QUIET_RADS = 1.5f;

        /// Clamp on each axis of the integral term [rad/s]: a real gyro bias
        /// stays well under this, anything larger is an attitude transient
        /// the integrator must not memorize. Bounds the standing attitude
        /// error to BIAS_LIMIT_RADS / kp even when everything else fails.
        static constexpr float BIAS_LIMIT_RADS = 0.05f;

        /// The integral only learns while the direction error is under this
        /// (sine of about 10 deg): a large error is the attitude converging
        /// (after a crash, a reset, a tumble), not bias information, and
        /// integrating it poisons the bias for the flights that follow.
        static constexpr float KI_ERROR_GATE_SIN = 0.17f;

        /// @param kp proportional gain on the gravity direction error [1/s]
        /// @param ki integral gain, drives the gyro bias estimate [1/s^2]
        explicit AttitudeEstimator(float kp = DEFAULT_KP, float ki = DEFAULT_KI)
            : m_kp(kp),
              m_ki(ki)
        {
        }

        /// @brief Sets the proportional gain. Callable between two steps; the
        ///        bias integrator is deliberately left untouched, it holds a
        ///        physical property of the gyro that no gain change invalidates.
        /// @param kp proportional gain on the gravity direction error [1/s]
        void setKp(float kp)
        {
            m_kp = kp;
        }

        /// @brief Sets the integral gain. Callable between two steps; the bias
        ///        integrator is deliberately left untouched, it holds a
        ///        physical property of the gyro that no gain change invalidates.
        /// @param ki integral gain, drives the gyro bias estimate [1/s^2]
        void setKi(float ki)
        {
            m_ki = ki;
        }

        /// @brief Advances the estimate with one sensor frame. The caller owns
        ///        the time policy (monotonicity, gap handling) and hands the
        ///        integration step down; a non-positive dt is a no-op.
        /// @param frame sensor frame carrying gyro and accel
        /// @param dtS integration step [s], 0 when nothing may integrate
        ///        (first frame or gap in the stream)
        /// @param allowAccelCorrection false while the caller is deliberately
        ///        accelerating (recovery, braking): thrust then dominates the
        ///        specific force at about 1 g along body up, which would pass
        ///        the gate and drag the estimate toward a false level. The
        ///        update is pure gyro integration instead.
        void update(const SensorFrame &frame, float dtS, bool allowAccelCorrection = true);

        /// @return body-to-world attitude
        [[nodiscard]] const Quaternion &attitude() const
        {
            return m_attitude;
        }

        /// @return estimated constant gyro bias [rad/s]
        [[nodiscard]] std::array<float, 3> gyroBiasRadS() const
        {
            return {-m_integralFb[0], -m_integralFb[1], -m_integralFb[2]};
        }

      private:
        float m_kp;                          ///< proportional gain [1/s]
        float m_ki;                          ///< integral gain [1/s^2]
        Quaternion m_attitude;               ///< body-to-world estimate
        std::array<float, 3> m_integralFb{}; ///< PI integral term = -gyro bias [rad/s]
    };
} // namespace mark4
