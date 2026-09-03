#pragma once

/// @file
/// @brief The plant's exact state as telemetry measures, and the one number
///        only a simulator can produce: how far the estimate is from the
///        truth. Sim only, by construction - a board has no plant behind it.

#include <array>
#include <cmath>
#include <cstddef>

#include "flight_core/types.hpp"
#include "protocol/envelope.hpp"
#include "telemetry/registry.hpp"

namespace mark4
{
    /// Mirrors the truth of the last frame into measures, and computes the
    /// attitude error next to them: the estimate and the truth are only both
    /// at hand in the composition root, which is where this is fed from.
    class TruthTelemetry
    {
      public:
        /// Half of the full turn in the error angle formula.
        static constexpr float ERROR_ANGLE_SCALE = 2.0f;

        /// @brief Takes the plant state of a frame and the estimate produced
        ///        for that same frame.
        /// @param truth exact state at the frame's instant
        /// @param estimate attitude the flight core estimated for it
        void update(const mark4_PlantTruth &truth, const Quaternion &estimate)
        {
            for (std::size_t axis = 0U; axis < m_positionM.size(); ++axis)
            {
                m_positionM[axis] = truth.position_m[axis];
                m_velocityMps[axis] = truth.velocity_mps[axis];
            }
            for (std::size_t index = 0U; index < m_attitude.size(); ++index)
            {
                m_attitude[index] = truth.attitude_quat[index];
            }
            // The rotation angle between two unit quaternions, the same
            // expression the ground tools use (quat.ts, telemetry_wire.py):
            // 2 * acos(|dot|), so a quaternion and its negation read as the
            // same attitude. The dot is clamped because two unit quaternions
            // stored as floats can dot to a hair over 1.
            const float dot = std::fabs(m_attitude[0] * estimate.w + m_attitude[1] * estimate.x +
                                        m_attitude[2] * estimate.y + m_attitude[3] * estimate.z);
            m_attitudeErrorRad = ERROR_ANGLE_SCALE * std::acos(dot > 1.0f ? 1.0f : dot);
        }

      private:
        std::array<float, 3> m_positionM{};   ///< exact world position [m]
        std::array<float, 3> m_velocityMps{}; ///< exact world velocity [m/s]
        std::array<float, 4> m_attitude{};    ///< exact attitude, w x y z
        float m_attitudeErrorRad = 0.0f;      ///< angle between estimate and truth [rad]

        TelemetryEntry m_positionX{"sim/truth/position_x", TelemetryUnit::M, m_positionM[0]};
        TelemetryEntry m_positionY{"sim/truth/position_y", TelemetryUnit::M, m_positionM[1]};
        TelemetryEntry m_positionZ{"sim/truth/position_z", TelemetryUnit::M, m_positionM[2]};
        TelemetryEntry m_velocityX{
            "sim/truth/velocity_x", TelemetryUnit::M_PER_S, m_velocityMps[0]};
        TelemetryEntry m_velocityY{
            "sim/truth/velocity_y", TelemetryUnit::M_PER_S, m_velocityMps[1]};
        TelemetryEntry m_velocityZ{
            "sim/truth/velocity_z", TelemetryUnit::M_PER_S, m_velocityMps[2]};
        TelemetryEntry m_quatW{"sim/truth/attitude/w", TelemetryUnit::UNITLESS, m_attitude[0]};
        TelemetryEntry m_quatX{"sim/truth/attitude/x", TelemetryUnit::UNITLESS, m_attitude[1]};
        TelemetryEntry m_quatY{"sim/truth/attitude/y", TelemetryUnit::UNITLESS, m_attitude[2]};
        TelemetryEntry m_quatZ{"sim/truth/attitude/z", TelemetryUnit::UNITLESS, m_attitude[3]};
        TelemetryEntry m_errorEntry{"sim/attitude_error", TelemetryUnit::RAD, m_attitudeErrorRad};
    };
} // namespace mark4
