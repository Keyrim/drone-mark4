#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/attitude_estimator.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/tuning_table.hpp"
#include "flight_core/vertical_controller.hpp"

namespace
{
    /// @return live value of a parameter, or NaN when the id is unknown
    float readParam(const mark4::TuningTable &table, std::uint16_t id)
    {
        float value = std::numeric_limits<float>::quiet_NaN();
        static_cast<void>(table.get(id, value));
        return value;
    }
} // namespace

TEST_CASE("the tuning registry is well formed")
{
    const mark4::TuningTable table;

    for (std::size_t index = 0U; index < mark4::TuningTable::PARAM_COUNT; ++index)
    {
        const mark4::TuningParam *param = table.info(index);
        REQUIRE(param != nullptr);

        // A zero id is never handed out: it is the "no parameter" value.
        REQUIRE(param->id != 0U);

        // Ids are unique, so a value can never be routed to two modules.
        for (std::size_t other = 0U; other < index; ++other)
        {
            REQUIRE(table.info(other)->id != param->id);
        }

        // Names are non-empty printable ASCII, zero padded to the field width.
        REQUIRE(param->name[0] != '\0');
        bool padding = false;
        for (const char c : param->name)
        {
            if (c == '\0')
            {
                padding = true;
            }
            else
            {
                REQUIRE(!padding); // no character after the padding starts
                REQUIRE(c >= 0x20);
                REQUIRE(c < 0x7F);
            }
        }

        // The default sits inside the range the table will enforce.
        REQUIRE(param->minValue <= param->maxValue);
        REQUIRE(param->value >= param->minValue);
        REQUIRE(param->value <= param->maxValue);
    }

    REQUIRE(table.info(mark4::TuningTable::PARAM_COUNT) == nullptr);
}

TEST_CASE("the registry defaults are the owning modules' constants")
{
    const mark4::TuningTable table;

    REQUIRE(readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH) ==
            mark4::RateController::DEFAULT_KP_ROLL_PITCH);
    REQUIRE(readParam(table, mark4::TUNING_ID_HOVER_COLLECTIVE) ==
            mark4::VerticalController::DEFAULT_HOVER_COLLECTIVE);
    REQUIRE(readParam(table, mark4::TUNING_ID_AHRS_KI) == mark4::AttitudeEstimator::DEFAULT_KI);
}

TEST_CASE("an unknown id is rejected and changes nothing")
{
    mark4::TuningTable table;
    const float before = readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH);

    REQUIRE(table.set(9999U, 1.0f, false) == mark4::TuningStatus::UNKNOWN_ID);

    float value = -1.0f;
    REQUIRE(table.get(9999U, value) == mark4::TuningStatus::UNKNOWN_ID);
    REQUIRE(value == -1.0f); // untouched on failure
    REQUIRE(readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH) == before);
}

TEST_CASE("the tuning bounds are inclusive and reject NaN")
{
    mark4::TuningTable table;
    const mark4::TuningParam *param = table.info(0U);
    REQUIRE(param != nullptr);
    const std::uint16_t id = param->id;
    const float minValue = param->minValue;
    const float maxValue = param->maxValue;
    const float original = param->value;

    // Outside the range, either way: refused, and the live value survives.
    REQUIRE(table.set(id, minValue - 1.0f, false) == mark4::TuningStatus::OUT_OF_BOUNDS);
    REQUIRE(readParam(table, id) == original);
    REQUIRE(table.set(id, maxValue + 1.0f, false) == mark4::TuningStatus::OUT_OF_BOUNDS);
    REQUIRE(readParam(table, id) == original);

    // A NaN passes no comparison, so the acceptance test rejects it.
    REQUIRE(table.set(id, std::numeric_limits<float>::quiet_NaN(), false) ==
            mark4::TuningStatus::OUT_OF_BOUNDS);
    REQUIRE(readParam(table, id) == original);

    // Both edges are valid values.
    REQUIRE(table.set(id, minValue, false) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, id) == minValue);
    REQUIRE(table.set(id, maxValue, false) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, id) == maxValue);
}

TEST_CASE("the armed lock only guards the parameters that ask for it")
{
    mark4::TuningTable table;

    // An estimator gain reinterprets the state the stack flies on: locked.
    REQUIRE(table.set(mark4::TUNING_ID_AHRS_KP, 3.0f, true) ==
            mark4::TuningStatus::LOCKED_WHILE_ARMED);
    REQUIRE(readParam(table, mark4::TUNING_ID_AHRS_KP) == mark4::AttitudeEstimator::DEFAULT_KP);

    // A controller gain is exactly what a pilot retunes in flight.
    REQUIRE(table.set(mark4::TUNING_ID_RATE_KP_ROLL_PITCH, 0.05f, true) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH) == 0.05f);

    // Disarmed, the locked one moves.
    REQUIRE(table.set(mark4::TUNING_ID_AHRS_KP, 3.0f, false) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, mark4::TUNING_ID_AHRS_KP) == 3.0f);
}
