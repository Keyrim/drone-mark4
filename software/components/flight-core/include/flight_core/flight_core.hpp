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
#include "telemetry/registry.hpp"

namespace mark4
{
    /// Phase of the flight state machine, and the piloting mode with it: the
    /// mode is not a second thing to publish, it IS which phase the machine
    /// sits in, so the one byte already on the wire describes the whole
    /// situation and no packet had to change to carry the modes.
    ///
    /// The arm switch is the single gate. No phase but IDLE and CUTOFF is
    /// reachable while it is off, and switching it off from any of them cuts
    /// the motors and returns to IDLE, whatever was in progress.
    ///
    /// Each mode leaves IDLE through its own interlock, and which mode was
    /// used is latched on the way out, so moving the mode switch afterwards
    /// changes nothing until the drone is disarmed again:
    /// - MANUAL wants the stick down. The motors follow the stick from zero,
    ///   so entering on a raised stick would be a jump to a live collective.
    /// - ALTITUDE_AUTO wants the stick centered, and leads to ARMED, not to
    ///   piloted flight. In that mode a centered stick means "hold this
    ///   altitude", which on the ground is a takeoff nobody asked for; so
    ///   ALTITUDE_AUTO piloted flight is only ever reached by taking over a
    ///   HOVER the drone flew itself into after a throw.
    enum class FlightPhase : std::uint8_t
    {
        IDLE = 0U,          ///< motors stopped, waiting for the pilot
        ALTITUDE_AUTO = 1U, ///< piloted flight: the stick commands a vertical velocity
        ARMED = 2U,         ///< armed for a throw: motors stopped, detector watched
        BALLISTIC = 3U,     ///< throw detected, coasting until the spin-up instant
        RECOVERY = 4U,      ///< motors on, leveling from an arbitrary attitude
        HOVER = 5U,         ///< recovered: altitude hold until the pilot takes over
        CUTOFF = 6U,        ///< safety cutoff latched: motors stopped until rearm
        MANUAL = 7U,        ///< piloted flight: the stick commands the collective
        FAULT = 8U,         ///< IMU lost with the motors running: stopped until the kill
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
        /// - Sensor health comes next, from the imuValid / baroValid flags the
        ///   platform sets per frame. An invalid IMU frame integrates nothing
        ///   (no attitude, no altitude, no throw detection) and never arms.
        ///   With the motors off it is a passing condition: valid frames
        ///   resume normal operation. With the motors on, IMU_FAULT_FRAMES
        ///   consecutive invalid frames latch FAULT: motors cut, the kill
        ///   switch is the only way out. Before the threshold the last
        ///   command is held. An invalid baro is never a fault: the vertical
        ///   estimate coasts on the accelerometer, arming is refused while
        ///   it lasts on the ground, a flight in progress continues.
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

        /// @brief Restores every module to its constructed state, the tuned
        ///        values and the counters included: a reset is a power cycle,
        ///        exactly what flash-less hardware does. A composition calls
        ///        it when the time base of its frames changed, when the
        ///        simulated world teleported, or on a reboot command.
        ///
        ///        What deliberately survives is the telemetry registration of
        ///        every module: names, units and addresses do not change, so
        ///        a ground tool streaming this node keeps the ids it pulled.
        void reset();

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

        /// @return last plausible pressure altitude above the startup
        ///         reference [m], the raw channel altitudeM() is corrected
        ///         toward
        [[nodiscard]] float baroAltitudeM() const
        {
            return m_verticalEstimator.baroAltitudeM();
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

        /// Throttle under which the stick counts as down: the interlock the
        /// direct-thrust mode is entered through, and the position it starts
        /// its collective from.
        static constexpr float STICK_DOWN_THROTTLE = 0.05f;

        /// Throttle above which the stick counts as raised again. The band
        /// between STICK_DOWN_THROTTLE and this is hysteresis: inside it the
        /// stick keeps its previous state, so RC noise sitting on the
        /// boundary cannot chatter the state machine at frame rate.
        static constexpr float STICK_UP_THROTTLE = 0.10f;

        /// Throttle a centered stick sits at.
        static constexpr float STICK_CENTER = 0.5f;

        /// Half-width of the band around STICK_CENTER read as centered. A
        /// spring-loaded stick never returns to the exact same point and the
        /// RC quantization adds its own jitter; inside this band the vertical
        /// velocity setpoint is exactly zero, so a released stick holds the
        /// altitude instead of drifting at a fraction of a m/s.
        static constexpr float STICK_CENTER_DEADBAND = 0.05f;

        /// Distance from STICK_CENTER at which a stick read as centered stops
        /// being read that way. Wider than the deadband on purpose: it is the
        /// hysteresis of the centered latch, so a stick resting on the edge
        /// of the deadband cannot flip the hover takeover at frame rate.
        static constexpr float STICK_CENTER_RELEASE = 0.08f;

        /// Vertical velocity setpoint at full stick deflection [m/s]; mid
        /// stick holds the altitude. The mapping is continuous: the
        /// deflection is measured from the edge of the deadband, so leaving
        /// the deadband starts at 0 m/s and full stick still reaches exactly
        /// this value.
        static constexpr float STICK_VZ_RANGE_MPS = 2.0f;

        /// Consecutive invalid IMU frames with the motors running before
        /// FAULT latches (20 ms at 500 Hz): a lone I2C glitch holds the
        /// command, a dead sensor cuts the motors before the drone tumbles.
        static constexpr std::uint32_t IMU_FAULT_FRAMES = 10U;

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

        /// @return mode the current mission left IDLE with; while IDLE, the
        ///         mode the last mission used, MANUAL after a reset
        [[nodiscard]] PilotMode pilotMode() const
        {
            return m_lockedMode;
        }

        /// @return true in every phase where the motors may run, ARMED
        ///         included: the drone is one detection away from flying
        ///         there. CUTOFF and FAULT are not armed - they are latched
        ///         motors off, which is precisely where retuning belongs.
        [[nodiscard]] bool armed() const
        {
            return m_phase != FlightPhase::IDLE && m_phase != FlightPhase::CUTOFF &&
                   m_phase != FlightPhase::FAULT;
        }

        /// @return consecutive frames stepped without a valid IMU, 0 after
        ///         a valid one
        [[nodiscard]] std::uint32_t imuInvalidRun() const
        {
            return m_imuInvalidRun;
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
        /// @brief Handles a frame without a valid IMU: nothing integrates,
        ///        the outputs hold or FAULT latches.
        void stepWithoutImu(const SensorFrame &sensors, ActuatorFrame &actuators);
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
        /// @return true when the entry cutoffs trip on this frame
        [[nodiscard]] bool cutoffTripped(const SensorFrame &sensors);

        FlightPhase m_phase = FlightPhase::IDLE;
        /* The two stick latches track the stick, not the mission: they are
           updated on every frame and deliberately survive resetMission(),
           because a stick that was down before a kill is still down after it.
           Resetting them would make the machine forget a position the pilot
           never moved, and the next gesture would be read as fresh. */
        bool m_stickCentered = false;               ///< centered state, with hysteresis
        PilotMode m_lockedMode = PilotMode::MANUAL; ///< mode captured on leaving IDLE
        bool m_takeoverArmed = false;               ///< the stick was centered since HOVER began
        bool m_stickDown = true;                    ///< throttle state, with hysteresis
        std::uint32_t m_handledThrowCount = 0U;     ///< throws already acted upon
        std::uint64_t m_recoveryStartUs = 0U;       ///< entry instant of RECOVERY [us]
        std::uint64_t m_hoverStartUs = 0U;          ///< entry instant of HOVER [us]
        bool m_brakeDone = false;                   ///< braking spent for this flight
        bool m_tiltExceeded = false;                ///< a tilt streak is in progress
        std::uint64_t m_tiltExceededSinceUs = 0U;   ///< start of the tilt streak [us]
        std::uint32_t m_stepCount = 0U;
        std::uint32_t m_staleFrameCount = 0U;   ///< frames with a non-increasing timestamp
        std::uint32_t m_invalidFrameCount = 0U; ///< frames with a NaN or Inf field
        std::uint32_t m_imuInvalidRun = 0U;     ///< consecutive frames without a valid IMU
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

        /// @param context the core the entry was built with
        /// @return the phase of the state machine as its numeric code
        static float ReadFlightPhase(const void *context);

        /// @param context the core the entry was built with
        /// @return the piloting mode as its numeric code
        static float ReadPilotMode(const void *context);

        // Measures. The four motor commands are the mixer's output as it
        // really left the core (m_lastMotor is what step() published); the
        // phase and the mode are enums and go through readers.
        TelemetryEntry m_motor0{"mixer/motor_0", TelemetryUnit::UNITLESS, m_lastMotor[0]};
        TelemetryEntry m_motor1{"mixer/motor_1", TelemetryUnit::UNITLESS, m_lastMotor[1]};
        TelemetryEntry m_motor2{"mixer/motor_2", TelemetryUnit::UNITLESS, m_lastMotor[2]};
        TelemetryEntry m_motor3{"mixer/motor_3", TelemetryUnit::UNITLESS, m_lastMotor[3]};
        TelemetryEntry m_phaseEntry{
            "core/flight_phase", TelemetryUnit::UNITLESS, this, &ReadFlightPhase};
        TelemetryEntry m_modeEntry{
            "core/pilot_mode", TelemetryUnit::UNITLESS, this, &ReadPilotMode};
    };
} // namespace mark4
