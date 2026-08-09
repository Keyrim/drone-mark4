#pragma once

/// @file
/// @brief Flight core entry point.

#include <array>
#include <cstddef>
#include <cstdint>

#include "flight_core/attitude_controller.hpp"
#include "flight_core/attitude_estimator.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/throw_detector.hpp"
#include "flight_core/tuning_table.hpp"
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
        IDLE = 0U,          ///< motors stopped, waiting for the pilot
        ALTITUDE_AUTO = 1U, ///< piloted flight: the stick commands a vertical velocity
        ARMED = 2U,         ///< armed for a throw: motors stopped, detector watched
        BALLISTIC = 3U,     ///< throw detected, coasting until the spin-up instant
        RECOVERY = 4U,      ///< motors on, leveling from an arbitrary attitude
        HOVER = 5U,         ///< recovered: altitude hold until the pilot takes over
        CUTOFF = 6U,        ///< safety cutoff latched: motors stopped until rearm
    };

    /// Synchronous, single-threaded flight core, paced by data arrival
    /// (never by time). Pure: no allocation, no waiting, no clock access.
    class FlightCore
    {
      public:
        /// @brief Runs one control step. The kill switch is honored first.
        ///
        /// Input contract. The kill switch is a level and is honored on every
        /// frame, whatever else the frame carries. Any other frame must be
        /// finite and move time forward, or it is rejected as a whole: the
        /// outputs hold their previous values and no internal state advances.
        /// - A NaN or Inf in any float field rejects the frame: NaN makes
        ///   every safety comparison false, so scrubbing at this boundary is
        ///   what keeps the cutoffs meaningful and the motors numeric. A lone
        ///   glitch frame costs nothing; a persistent stream of them leaves
        ///   the motors on their last command, and the RC kill (silence means
        ///   kill) stays the way out, as for any other frozen input.
        /// - A timestamp not strictly above the last accepted one is a
        ///   transport artifact: rejected, and the time reference does not
        ///   move (a rebased stream is a new session; the composition signals
        ///   it with a fresh core, it is never inferred here).
        /// - Frames further apart than MAX_STEP_S are gaps: the step runs but
        ///   nothing integrates over the hole. step() derives dt once under
        ///   this policy and passes it down; no other module does timestamp
        ///   arithmetic.
        /// - The state machine never leaves IDLE before the vertical
        ///   estimator captured its baro reference, which takes
        ///   REFERENCE_SAMPLES resting frames: a dead baro or a boot in
        ///   motion keeps the motors off. Implausible or glitched baro
        ///   samples afterwards are the estimator's business (plausibility
        ///   window, innovation clamp).
        /// @param sensors latest sensor frame
        /// @param[out] actuators motor commands computed for this step
        void step(const SensorFrame &sensors, ActuatorFrame &actuators);

        /// @return number of steps executed since construction
        [[nodiscard]] std::uint32_t stepCount() const
        {
            return m_stepCount;
        }

        /// @return frames rejected for a non-increasing timestamp
        [[nodiscard]] std::uint32_t staleFrameCount() const
        {
            return m_staleFrameCount;
        }

        /// @return frames rejected for a non-finite float field (NaN or Inf)
        [[nodiscard]] std::uint32_t invalidFrameCount() const
        {
            return m_invalidFrameCount;
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

        /// @brief Validates a new value for a tunable parameter and, when it
        ///        is accepted, applies it to the module that owns it in the
        ///        same call - the registry and the modules never disagree, so
        ///        there is no window where a reader sees a value that is not
        ///        flying yet.
        ///
        ///        The core is single-threaded: call this from the thread that
        ///        runs step(), between two steps. The new value is then in
        ///        effect for the whole of the next step. Integrator state
        ///        deliberately survives a gain change, because retuning a loop
        ///        mid-flight must not kick it.
        /// @param id parameter identifier
        /// @param value candidate value
        /// @return OK when the value was applied, the reason otherwise
        TuningStatus setParam(std::uint16_t id, float value);

        /// @brief Reads the live value of a tunable parameter.
        /// @param id parameter identifier
        /// @param[out] valueOut live value, written only when the call returns OK
        /// @return OK, or UNKNOWN_ID when no parameter carries this id
        [[nodiscard]] TuningStatus getParam(std::uint16_t id, float &valueOut) const;

        /// @return number of tunable parameters
        [[nodiscard]] static constexpr std::size_t ParamCount()
        {
            return TuningTable::PARAM_COUNT;
        }

        /// @brief Enumerates the tunable parameters, name and bounds included,
        ///        so a ground station discovers them instead of hardcoding them.
        /// @param index position in the registry, in [0, ParamCount())
        /// @return the entry, or nullptr past the end
        [[nodiscard]] const TuningParam *paramInfo(std::size_t index) const;

        /// Throttle below which the drone stays disarmed: motors stopped and
        /// control integrators held at zero.
        static constexpr float ARM_THROTTLE = 0.05f;

        /// Throttle above which the stick counts as raised again. The band
        /// between ARM_THROTTLE and this is hysteresis: inside it the stick
        /// keeps its previous state, so RC noise sitting on the boundary
        /// cannot chatter the state machine (arming, hover takeover, cutoff
        /// release) at frame rate.
        static constexpr float STICK_UP_THROTTLE = 0.10f;

        /// Throttle a centered stick sits at.
        static constexpr float STICK_CENTER = 0.5f;

        /// Half-width of the band around STICK_CENTER read as centered. A
        /// spring-loaded stick never returns to the exact same point and the
        /// RC quantization adds its own jitter; inside this band the vertical
        /// velocity setpoint is exactly zero, so a released stick holds the
        /// altitude instead of drifting at a fraction of a m/s.
        static constexpr float STICK_CENTER_DEADBAND = 0.05f;

        /// Vertical velocity setpoint at full stick deflection [m/s]; mid
        /// stick holds the altitude. The mapping is continuous: the
        /// deflection is measured from the edge of the deadband, so leaving
        /// the deadband starts at 0 m/s and full stick still reaches exactly
        /// this value.
        static constexpr float STICK_VZ_RANGE_MPS = 2.0f;

        /// Frames further apart than this are gaps in the stream: the step
        /// still runs but dt is forced to 0, so no estimator or control
        /// integrator ever integrates the hole. Owned here, for every module.
        static constexpr float MAX_STEP_S = 0.05f;

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

        /// @brief Maps a stick position to the vertical velocity it commands,
        ///        deadband included.
        /// @param throttle normalized stick position [0, 1]
        /// @return vertical velocity setpoint, positive up [m/s]
        [[nodiscard]] static float StickVerticalVelocityMps(float throttle);

      private:
        /// @return true when the timestamp moves time forward (or is the first)
        [[nodiscard]] bool acceptsTimestamp(std::uint64_t timestampUs) const;
        /// @brief Advances the time reference and derives the integration step.
        /// @return dt [s], 0 on the first frame or across a gap
        float deriveDt(std::uint64_t timestampUs);
        /// @brief Ends the mission: resets everything mission-scoped and
        ///        returns to IDLE. What survives, survives by design: the
        ///        estimators keep tracking (fresh attitude on rearm), the
        ///        throw detector keeps counting (the arm-time snapshot
        ///        consumes pre-arm throws), the counters and the time
        ///        reference keep their monotonic history.
        void resetMission();
        void updateEstimators(const SensorFrame &sensors, float dt);
        void advancePhase(const SensorFrame &sensors);
        void runControl(const SensorFrame &sensors, float dt, ActuatorFrame &actuators);
        /// @return true when the accel norm says impact
        [[nodiscard]] static bool ImpactTripped(const SensorFrame &sensors);
        /// @return true when the gyro sits near its full scale
        [[nodiscard]] static bool GyroSaturated(const SensorFrame &sensors);
        /// @brief Advances the sustained-tilt streak with this frame.
        /// @return true once the excessive tilt lasted CUTOFF_TILT_CONFIRM_US
        [[nodiscard]] bool tiltCutoffConfirmed(const SensorFrame &sensors);
        [[nodiscard]] float estimatedUpZ() const;
        [[nodiscard]] std::array<float, 3> brakeUpWorld() const;
        void applyParam(std::uint16_t id, float value);

        FlightPhase m_phase = FlightPhase::IDLE;
        bool m_stickDown = true;                  ///< throttle state, with hysteresis
        std::uint32_t m_handledThrowCount = 0U;   ///< throws already acted upon
        std::uint64_t m_recoveryStartUs = 0U;     ///< entry instant of RECOVERY [us]
        std::uint64_t m_hoverStartUs = 0U;        ///< entry instant of HOVER [us]
        bool m_brakeDone = false;                 ///< braking spent for this flight
        bool m_tiltExceeded = false;              ///< a tilt streak is in progress
        std::uint64_t m_tiltExceededSinceUs = 0U; ///< start of the tilt streak [us]
        std::uint32_t m_stepCount = 0U;
        std::uint32_t m_staleFrameCount = 0U;   ///< frames with a non-increasing timestamp
        std::uint32_t m_invalidFrameCount = 0U; ///< frames with a NaN or Inf field
        std::array<float, 4> m_lastMotor{};     ///< outputs held when a frame is rejected
        AttitudeEstimator m_attitudeEstimator;
        VerticalEstimator m_verticalEstimator;
        ThrowDetector m_throwDetector;
        AttitudeController m_attitudeController;
        RateController m_rateController;
        VerticalController m_verticalController;
        std::uint64_t m_prevTimestampUs = 0U; ///< timestamp of the last accepted frame [us]
        bool m_hasPrevTimestamp = false;      ///< false until the first accepted frame
        TuningTable m_tuning;                 ///< registry of the tunable gains
    };
} // namespace mark4
