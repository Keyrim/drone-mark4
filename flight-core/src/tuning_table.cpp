#include "flight_core/tuning_table.hpp"

#include "flight_core/attitude_controller.hpp"
#include "flight_core/attitude_estimator.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/vertical_controller.hpp"
#include "flight_core/vertical_estimator.hpp"

namespace mark4
{
    namespace
    {
        /// @brief Builds a name field out of a literal, zero padded and left
        ///        unterminated when it fills the field exactly.
        /// @param text ASCII name, at most TuningParam::NAME_SIZE characters
        /// @return the padded name field
        template <std::size_t N>
        consteval std::array<char, TuningParam::NAME_SIZE> makeName(const char (&text)[N])
        {
            // N counts the literal's terminator, the name itself is N - 1 long.
            static_assert(N - 1U <= TuningParam::NAME_SIZE, "tuning parameter name is too long");
            std::array<char, TuningParam::NAME_SIZE> name{};
            for (std::size_t i = 0U; i + 1U < N; ++i)
            {
                name[i] = text[i];
            }
            return name;
        }
    } // namespace

    // The bounds below are the only literals of this table: every default
    // value comes from the constant the owning module already publishes, which
    // stays the single source of truth. The bounds are new information (the
    // range a pilot may explore without breaking the loop) and there is no
    // clearer way to write them than in place, next to the value they frame.
    // NOLINTBEGIN(readability-magic-numbers)
    TuningTable::TuningTable()
        : m_params{{
              {TUNING_ID_RATE_KP_ROLL_PITCH,
               makeName("rate_kp_rp"),
               RateController::DEFAULT_KP_ROLL_PITCH,
               0.0f,
               0.5f,
               true},
              {TUNING_ID_RATE_KI_ROLL_PITCH,
               makeName("rate_ki_rp"),
               RateController::DEFAULT_KI_ROLL_PITCH,
               0.0f,
               0.5f,
               true},
              {TUNING_ID_RATE_KP_YAW,
               makeName("rate_kp_yaw"),
               RateController::DEFAULT_KP_YAW,
               0.0f,
               1.0f,
               true},
              {TUNING_ID_RATE_KI_YAW,
               makeName("rate_ki_yaw"),
               RateController::DEFAULT_KI_YAW,
               0.0f,
               1.0f,
               true},
              {TUNING_ID_ATTITUDE_KP,
               makeName("att_kp"),
               AttitudeController::DEFAULT_KP,
               0.0f,
               20.0f,
               true},
              {TUNING_ID_VERTICAL_KP,
               makeName("vz_kp"),
               VerticalController::DEFAULT_KP,
               0.0f,
               1.0f,
               true},
              {TUNING_ID_VERTICAL_KI,
               makeName("vz_ki"),
               VerticalController::DEFAULT_KI,
               0.0f,
               1.0f,
               true},
              // Capped well under a full command: the mixer spreads the
              // collective over the four motors and then adds the torque
              // demands, so a hover feedforward that already saturates them
              // leaves the attitude loops no authority at all.
              {TUNING_ID_HOVER_COLLECTIVE,
               makeName("hover_coll"),
               VerticalController::DEFAULT_HOVER_COLLECTIVE,
               0.0f,
               0.9f,
               true},
              // The estimator gains stay locked while armed: they do not shape
              // a command, they reinterpret the state the whole stack flies on,
              // and moving them mid-flight makes the attitude jump under the
              // controllers rather than merely retuning them.
              {TUNING_ID_AHRS_KP,
               makeName("ahrs_kp"),
               AttitudeEstimator::DEFAULT_KP,
               0.0f,
               10.0f,
               false},
              {TUNING_ID_AHRS_KI,
               makeName("ahrs_ki"),
               AttitudeEstimator::DEFAULT_KI,
               0.0f,
               1.0f,
               false},
              {TUNING_ID_VEST_ALTITUDE_GAIN,
               makeName("vest_alt_gain"),
               VerticalEstimator::DEFAULT_ALTITUDE_GAIN,
               0.0f,
               10.0f,
               false},
              {TUNING_ID_VEST_VELOCITY_GAIN,
               makeName("vest_vel_gain"),
               VerticalEstimator::DEFAULT_VELOCITY_GAIN,
               0.0f,
               20.0f,
               false},
          }}
    {
    }
    // NOLINTEND(readability-magic-numbers)

    TuningStatus TuningTable::set(std::uint16_t id, float value, bool armed)
    {
        TuningParam *param = find(id);
        if (param == nullptr)
        {
            return TuningStatus::UNKNOWN_ID;
        }
        if (armed && !param->armedChange)
        {
            return TuningStatus::LOCKED_WHILE_ARMED;
        }
        // Written as the negation of the accepting form on purpose: a NaN
        // compares false against everything, so it fails the acceptance and is
        // rejected, where the naive (value < min || value > max) would let it
        // straight through into a controller.
        if (!(value >= param->minValue && value <= param->maxValue))
        {
            return TuningStatus::OUT_OF_BOUNDS;
        }
        param->value = value;
        return TuningStatus::OK;
    }

    TuningStatus TuningTable::get(std::uint16_t id, float &valueOut) const
    {
        const TuningParam *param = find(id);
        if (param == nullptr)
        {
            return TuningStatus::UNKNOWN_ID;
        }
        valueOut = param->value;
        return TuningStatus::OK;
    }

    const TuningParam *TuningTable::info(std::size_t index) const
    {
        if (index >= m_params.size())
        {
            return nullptr;
        }
        return &m_params[index];
    }

    TuningParam *TuningTable::find(std::uint16_t id)
    {
        // Linear scan: a dozen entries, and a set() happens between two control
        // steps at human pace, never inside the loop's arithmetic.
        for (TuningParam &param : m_params)
        {
            if (param.id == id)
            {
                return &param;
            }
        }
        return nullptr;
    }

    const TuningParam *TuningTable::find(std::uint16_t id) const
    {
        for (const TuningParam &param : m_params)
        {
            if (param.id == id)
            {
                return &param;
            }
        }
        return nullptr;
    }
} // namespace mark4
