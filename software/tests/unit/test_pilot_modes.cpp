/// @file
/// @brief The two piloting modes: how each is entered, what the stick means
///        once it is, and everything that must never happen in between.

#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream

    /// Plausible static pressure carried by every helper frame: the default
    /// SensorFrame reads 0 Pa, a faulted sensor, and a core fed with it
    /// refuses to fly at all.
    constexpr float HELPER_BARO_PA = 101325.0f;

    /// One RC state, held for as many frames as a test needs.
    struct Stick
    {
        bool arm = false;                                 ///< arm switch
        mark4::PilotMode mode = mark4::PilotMode::MANUAL; ///< mode switch
        float throttle = 0.0f;                            ///< stick position [0, 1]
    };

    /// Drives one core with a scripted RC state, at rest and level unless a
    /// test says otherwise. Owns the timestamp so no test has to.
    class Pilot
    {
      public:
        /// @brief Steps the core the given number of frames on one RC state.
        /// @param stick RC state held for those frames
        /// @param steps number of frames
        void hold(const Stick &stick, std::uint32_t steps)
        {
            for (std::uint32_t i = 0U; i < steps; ++i)
            {
                mark4::SensorFrame frame;
                frame.timestampUs = m_timestampUs;
                frame.rc.killSwitch = m_kill;
                frame.rc.armSwitch = stick.arm;
                frame.rc.mode = stick.mode;
                frame.rc.throttle = stick.throttle;
                frame.gyroRadS = m_gyroRadS;
                frame.accelMps2 = m_accelMps2;
                frame.baroPa = HELPER_BARO_PA;
                frame.imuValid = true;
                frame.baroValid = true;
                m_core.step(frame, m_actuators);
                m_timestampUs += STEP_US;
            }
        }

        /// @brief Settles the estimators and the baro reference on the ground.
        /// @param stick RC state held while settling
        void settle(const Stick &stick)
        {
            hold(stick, 200U);
        }

        /// @brief Settles disarmed, arms, then plays a hand throw and flies
        ///        it all the way to the hover.
        /// @param stick armed RC state held from the arming on
        void throwToHover(const Stick &stick)
        {
            settle({false, stick.mode, stick.throttle});
            hold(stick, 1U);
            m_accelMps2 = {0.0f, 0.0f, 5.0f * mark4::GRAVITY_MPS2};
            hold(stick, 50U);
            m_accelMps2 = {0.0f, 0.0f, 0.0f};
            for (std::uint32_t i = 0U; i < 1500U && phase() != mark4::FlightPhase::HOVER; ++i)
            {
                hold(stick, 1U);
            }
            m_accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
        }

        /// @param kill true engages the kill switch on the frames that follow
        void setKill(bool kill)
        {
            m_kill = kill;
        }

        /// @param accelMps2 specific force carried by the frames that follow
        void setAccel(const std::array<float, 3> &accelMps2)
        {
            m_accelMps2 = accelMps2;
        }

        /// @param gyroRadS body rates carried by the frames that follow
        void setGyro(const std::array<float, 3> &gyroRadS)
        {
            m_gyroRadS = gyroRadS;
        }

        /// @return current phase of the flight state machine
        [[nodiscard]] mark4::FlightPhase phase() const
        {
            return m_core.flightPhase();
        }

        /// @return motor commands of the last step
        [[nodiscard]] const std::array<float, 4> &motor() const
        {
            return m_actuators.motor;
        }

        /// @return largest motor command of the last step
        [[nodiscard]] float maxMotor() const
        {
            float largest = 0.0f;
            for (const float m : m_actuators.motor)
            {
                largest = m > largest ? m : largest;
            }
            return largest;
        }

        /// @return the core being driven
        [[nodiscard]] mark4::FlightCore &accessCore()
        {
            return m_core;
        }

      private:
        mark4::FlightCore m_core;
        mark4::ActuatorFrame m_actuators;
        std::uint64_t m_timestampUs = STEP_US;
        bool m_kill = false;
        std::array<float, 3> m_gyroRadS{};
        std::array<float, 3> m_accelMps2{0.0f, 0.0f, mark4::GRAVITY_MPS2};
    };

    /// RC state of a drone sitting on the ground, disarmed, stick down.
    constexpr Stick GROUND{false, mark4::PilotMode::MANUAL, 0.0f};

    /// RC state of a drone sitting on the ground with the altitude-auto mode
    /// selected and the stick centered: the interlock, arm switch still off.
    constexpr Stick GROUND_AUTO{false, mark4::PilotMode::ALTITUDE_AUTO, 0.5f};

    /// The same, arm switch on: armed for a throw, stick centered.
    constexpr Stick ARMED_AUTO{true, mark4::PilotMode::ALTITUDE_AUTO, 0.5f};
} // namespace

TEST_CASE("the safe rc defaults never arm anything")
{
    // A zeroed RcInput is what the fail-safe grafts: kill engaged, disarmed,
    // manual, stick at the bottom. Not one frame of it may reach the motors.
    Pilot pilot;
    pilot.setKill(true);
    pilot.settle(GROUND);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);

    // Even with the kill released, the arm switch alone gates everything.
    pilot.setKill(false);
    pilot.hold(GROUND, 200U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    REQUIRE(pilot.maxMotor() == 0.0f);
}

TEST_CASE("the direct-thrust interlock demands the stick down")
{
    Pilot pilot;
    pilot.settle(GROUND);

    // Stick down, mode manual, switch on: flight, motors following the stick.
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.04f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);

    // A fresh core with the stick already up refuses: entering there would
    // be a jump straight to a live collective.
    Pilot raised;
    raised.settle(GROUND);
    raised.hold({true, mark4::PilotMode::MANUAL, 0.5f}, 5U);
    REQUIRE(raised.phase() == mark4::FlightPhase::IDLE);
    REQUIRE(raised.maxMotor() == 0.0f);
}

TEST_CASE("the stick-down boundary has hysteresis and cannot chatter")
{
    Pilot pilot;
    pilot.settle(GROUND);
    const Stick armedManual{true, mark4::PilotMode::MANUAL, 0.0f};
    pilot.hold(armedManual, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);

    // Disarm, then noise sitting inside the hysteresis band: the stick keeps
    // the state it had, so the drone never re-enters flight on noise alone.
    pilot.hold(GROUND, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    for (std::uint32_t i = 0U; i < 10U; ++i)
    {
        pilot.hold({true, mark4::PilotMode::MANUAL, i % 2U == 0U ? 0.06f : 0.09f}, 1U);
    }
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);

    // The other direction: past the release threshold the stick is up for
    // good, and disarming then rearming on that stick is refused.
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.12f}, 1U);
    pilot.hold({false, mark4::PilotMode::MANUAL, 0.12f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    for (std::uint32_t i = 0U; i < 10U; ++i)
    {
        pilot.hold({true, mark4::PilotMode::MANUAL, i % 2U == 0U ? 0.09f : 0.06f}, 1U);
        REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    }

    // Only a deliberate return under the down threshold arms again.
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.04f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);
}

TEST_CASE("the direct-thrust stick is the collective, and zero means zero")
{
    auto motorFor = [](float throttle) {
        Pilot pilot;
        pilot.settle(GROUND);
        pilot.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
        REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);
        pilot.hold({true, mark4::PilotMode::MANUAL, throttle}, 5U);
        REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);
        return pilot.motor()[0];
    };

    // Bottom of the stick is exactly stopped, not "nearly": on the ground
    // that is the difference between still props and props that creep.
    REQUIRE(motorFor(0.0f) == 0.0f);
    REQUIRE(motorFor(0.2f) > 0.0f);
    REQUIRE(motorFor(0.5f) > motorFor(0.2f));
    REQUIRE(motorFor(0.9f) > motorFor(0.5f));

    // The leveling still runs underneath: a standing roll rate splits the
    // left and right motors even though the collective is a raw stick value.
    Pilot rolling;
    rolling.settle(GROUND);
    rolling.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    rolling.setGyro({0.5f, 0.0f, 0.0f});
    rolling.hold({true, mark4::PilotMode::MANUAL, 0.5f}, 20U);
    REQUIRE(rolling.phase() == mark4::FlightPhase::MANUAL);
    REQUIRE(std::fabs(rolling.motor()[0] - rolling.motor()[2]) > 0.001f);
}

TEST_CASE("the altitude-auto interlock demands a centered stick")
{
    Pilot centered;
    centered.settle(GROUND_AUTO);
    centered.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.5f}, 1U);
    REQUIRE(centered.phase() == mark4::FlightPhase::ARMED);

    // Just outside the centre band: refused, and the motors stay stopped.
    Pilot offCentre;
    offCentre.settle({false, mark4::PilotMode::ALTITUDE_AUTO, 0.44f});
    offCentre.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.44f}, 5U);
    REQUIRE(offCentre.phase() == mark4::FlightPhase::IDLE);
    REQUIRE(offCentre.maxMotor() == 0.0f);

    // Just inside it: armed. The band is symmetric around mid stick.
    Pilot inside;
    inside.settle({false, mark4::PilotMode::ALTITUDE_AUTO, 0.46f});
    inside.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.46f}, 1U);
    REQUIRE(inside.phase() == mark4::FlightPhase::ARMED);
}

TEST_CASE("the centered latch has hysteresis of its own")
{
    Pilot pilot;
    pilot.settle(GROUND_AUTO);
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.5f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::ARMED);

    // Disarm, drift to the far side of the deadband but inside the release
    // distance: the stick still counts as centered, so rearming works.
    pilot.hold({false, mark4::PilotMode::ALTITUDE_AUTO, 0.57f}, 1U);
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.57f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::ARMED);

    // Past the release distance the latch drops, and rearming is refused
    // until the stick comes back inside the deadband.
    pilot.hold({false, mark4::PilotMode::ALTITUDE_AUTO, 0.59f}, 1U);
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.59f}, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.57f}, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.52f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::ARMED);
}

TEST_CASE("a throw in direct-thrust mode is not a mission")
{
    // The throw detector keeps running in every mode, but only a drone armed
    // in altitude-auto ever acts on what it sees. Thrown by hand while flying
    // manually, the machine must not leave the mode it was given.
    Pilot pilot;
    const Stick manual{true, mark4::PilotMode::MANUAL, 0.0f};
    pilot.settle(GROUND);
    pilot.hold(manual, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);

    pilot.setAccel({0.0f, 0.0f, 5.0f * mark4::GRAVITY_MPS2});
    pilot.hold(manual, 50U);
    pilot.setAccel({0.0f, 0.0f, 0.0f});
    pilot.hold(manual, 600U);

    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);
    REQUIRE(pilot.maxMotor() == 0.0f);
}

TEST_CASE("the mode switch is ignored once the altitude-auto mission started")
{
    Pilot pilot;
    pilot.settle(GROUND_AUTO);
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.5f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::ARMED);
    REQUIRE(pilot.accessCore().pilotMode() == mark4::PilotMode::ALTITUDE_AUTO);

    // The pilot bumps the mode switch mid-mission. The throw flight has to
    // complete exactly as if nothing had moved.
    const Stick bumped{true, mark4::PilotMode::MANUAL, 0.5f};
    pilot.setAccel({0.0f, 0.0f, 5.0f * mark4::GRAVITY_MPS2});
    pilot.hold(bumped, 50U);
    pilot.setAccel({0.0f, 0.0f, 0.0f});
    for (std::uint32_t i = 0U; i < 1500U && pilot.phase() != mark4::FlightPhase::HOVER; ++i)
    {
        pilot.hold(bumped, 1U);
    }
    REQUIRE(pilot.phase() == mark4::FlightPhase::HOVER);
    REQUIRE(pilot.accessCore().pilotMode() == mark4::PilotMode::ALTITUDE_AUTO);

    // Disarming ends the mission; only then does the new mode take.
    pilot.setAccel({0.0f, 0.0f, mark4::GRAVITY_MPS2});
    pilot.hold({false, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);
    REQUIRE(pilot.accessCore().pilotMode() == mark4::PilotMode::MANUAL);
}

TEST_CASE("the mode switch is ignored once the direct-thrust mission started")
{
    Pilot pilot;
    pilot.settle(GROUND);
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);

    // Selecting altitude-auto mid-flight with the stick at the bottom: if the
    // switch were obeyed, the closed loop would take over and push the motors
    // toward the hover collective. They must stay at exactly zero instead -
    // that is the observable proving the mode did not move.
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.0f}, 200U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);
    REQUIRE(pilot.maxMotor() == 0.0f);
    REQUIRE(pilot.accessCore().pilotMode() == mark4::PilotMode::MANUAL);
}

TEST_CASE("taking a hover over needs a recentring gesture")
{
    // A stick held at the centre forever never grabs the drone: the hover
    // holds for as long as the pilot leaves it alone.
    Pilot patient;
    patient.throwToHover(ARMED_AUTO);
    REQUIRE(patient.phase() == mark4::FlightPhase::HOVER);
    patient.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.5f}, 500U);
    REQUIRE(patient.phase() == mark4::FlightPhase::HOVER);

    // Recentre (already done above), then move: that is the takeover, and the
    // small deflection asks for a gentle climb rather than a jump.
    patient.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.6f}, 1U);
    REQUIRE(patient.phase() == mark4::FlightPhase::ALTITUDE_AUTO);
    REQUIRE(mark4::FlightCore::StickVerticalVelocityMps(0.6f) > 0.0f);
    REQUIRE(mark4::FlightCore::StickVerticalVelocityMps(0.6f) < 0.5f);

    // A stick parked at 0.9 for the whole flight never was centered, so it
    // never arms the takeover: the drone keeps its own hover.
    Pilot grabby;
    grabby.throwToHover(ARMED_AUTO);
    REQUIRE(grabby.phase() == mark4::FlightPhase::HOVER);
    grabby.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.9f}, 500U);
    REQUIRE(grabby.phase() == mark4::FlightPhase::HOVER);
}

TEST_CASE("the stick to vertical velocity map is exact, continuous and full range")
{
    const float deadband = mark4::FlightCore::STICK_CENTER_DEADBAND;
    const float centre = mark4::FlightCore::STICK_CENTER;
    const float range = mark4::FlightCore::STICK_VZ_RANGE_MPS;

    // Exactly zero everywhere inside the band, on both sides of the centre.
    REQUIRE(mark4::FlightCore::StickVerticalVelocityMps(centre) == 0.0f);
    REQUIRE(mark4::FlightCore::StickVerticalVelocityMps(centre + deadband - 0.001f) == 0.0f);
    REQUIRE(mark4::FlightCore::StickVerticalVelocityMps(centre - deadband + 0.001f) == 0.0f);

    // Continuous where it leaves the band: a hair outside is a hair of speed,
    // never a step.
    const float justOut = mark4::FlightCore::StickVerticalVelocityMps(centre + deadband + 0.001f);
    REQUIRE(justOut > 0.0f);
    REQUIRE(justOut < 0.05f);

    // Full deflection reaches exactly the declared range, both ways.
    REQUIRE(mark4::FlightCore::StickVerticalVelocityMps(1.0f) == range);
    REQUIRE(mark4::FlightCore::StickVerticalVelocityMps(0.0f) == -range);

    // Monotone and antisymmetric in between.
    float previous = -range;
    for (std::uint32_t i = 0U; i <= 100U; ++i)
    {
        const float throttle = static_cast<float>(i) / 100.0f;
        const float vz = mark4::FlightCore::StickVerticalVelocityMps(throttle);
        REQUIRE(vz >= previous);
        REQUIRE(vz <= range);
        REQUIRE(vz >= -range);
        previous = vz;
        REQUIRE(std::fabs(vz + mark4::FlightCore::StickVerticalVelocityMps(1.0f - throttle)) <
                1e-6f);
    }
}

TEST_CASE("the kill switch ends a direct-thrust flight")
{
    Pilot pilot;
    const Stick flying{true, mark4::PilotMode::MANUAL, 0.5f};
    pilot.settle(GROUND);
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    pilot.hold(flying, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);
    REQUIRE(pilot.maxMotor() > 0.0f);

    pilot.setKill(true);
    pilot.hold(flying, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    REQUIRE(pilot.maxMotor() == 0.0f);

    // Releasing the kill on a raised stick must not resume anything: the
    // interlock wants the stick down again.
    pilot.setKill(false);
    pilot.hold(flying, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    REQUIRE(pilot.maxMotor() == 0.0f);
}

TEST_CASE("the kill switch ends a taken-over altitude-auto flight")
{
    Pilot pilot;
    pilot.throwToHover(ARMED_AUTO);
    REQUIRE(pilot.phase() == mark4::FlightPhase::HOVER);
    // Recentre once to arm the takeover, then move off centre to take it.
    pilot.hold(ARMED_AUTO, 1U);
    const Stick flying{true, mark4::PilotMode::ALTITUDE_AUTO, 0.6f};
    pilot.hold(flying, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::ALTITUDE_AUTO);
    REQUIRE(pilot.maxMotor() > 0.0f);

    pilot.setKill(true);
    pilot.hold(flying, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    REQUIRE(pilot.maxMotor() == 0.0f);

    // The mission is over: the mode is back to the safe one and the drone
    // will not fly again without a fresh interlock.
    REQUIRE(pilot.accessCore().pilotMode() == mark4::PilotMode::MANUAL);
    pilot.setKill(false);
    pilot.hold(flying, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
}

TEST_CASE("a cutoff releases on the arm switch and rechecks the interlock")
{
    Pilot pilot;
    pilot.settle(GROUND);
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::MANUAL);

    // Saturated rates latch the cutoff.
    pilot.setGyro({70.0f, 0.0f, 0.0f});
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.5f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::CUTOFF);
    pilot.setGyro({0.0f, 0.0f, 0.0f});

    // Lowering the stick is no longer the release gesture: the arm switch is.
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::CUTOFF);
    pilot.hold({false, mark4::PilotMode::MANUAL, 0.5f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);

    // And rearming goes through the per-mode interlock again: a raised stick
    // in manual is refused, the same stick in altitude-auto arms for a throw.
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.5f}, 5U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::IDLE);
    pilot.hold({true, mark4::PilotMode::ALTITUDE_AUTO, 0.5f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::ARMED);
}

TEST_CASE("entering direct-thrust flight under absurd readings goes straight to cutoff")
{
    // The entry guard is what stops a takeoff attempt on a drone whose
    // sensors are already saying something impossible: the motors never turn
    // at all, rather than turning for one frame and then being cut.
    Pilot pilot;
    pilot.settle(GROUND);
    pilot.setGyro({70.0f, 0.0f, 0.0f});
    pilot.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    REQUIRE(pilot.phase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(pilot.maxMotor() == 0.0f);

    // An impact reading does the same.
    Pilot hit;
    hit.settle(GROUND);
    hit.setAccel({0.0f, 0.0f, 10.0f * mark4::GRAVITY_MPS2});
    hit.hold({true, mark4::PilotMode::MANUAL, 0.0f}, 1U);
    REQUIRE(hit.phase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(hit.maxMotor() == 0.0f);
}
