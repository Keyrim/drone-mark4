#include "flight_core/stick_mapper.hpp"

#include <cmath>

namespace mark4
{
    namespace
    {
        /// Horizontal heading norm under which the nose points straight up
        /// or down and the heading is undefined: the lean is then applied
        /// along the world axes, a choice that only matters for the frames
        /// of a vertical drone.
        constexpr float MIN_HEADING_NORM = 1e-3f;
    } // namespace

    StickMapper::StickMapper(float rateRollPitchRadS,
                             float rateYawRadS,
                             float tiltMaxRad,
                             float deadband)
        : m_rateRollPitchRadS(rateRollPitchRadS),
          m_rateYawRadS(rateYawRadS),
          m_tiltMaxRad(tiltMaxRad),
          m_tanTiltMax(std::tan(tiltMaxRad)),
          m_deadband(deadband)
    {
    }

    void StickMapper::setTiltMax(float tiltRad)
    {
        m_tiltMaxRad = tiltRad;
        m_tanTiltMax = std::tan(tiltRad);
    }

    float StickMapper::deflection(float stick) const
    {
        const float magnitude = stick < 0.0f ? -stick : stick;
        if (magnitude <= m_deadband)
        {
            return 0.0f;
        }
        // Measured from the edge of the band so the ramp leaves at exactly
        // zero, and scaled so a full stick still reaches exactly one.
        float scaled = (magnitude - m_deadband) / (1.0f - m_deadband);
        scaled = scaled > 1.0f ? 1.0f : scaled;
        return stick < 0.0f ? -scaled : scaled;
    }

    std::array<float, 3> StickMapper::rateSetpointRadS(const RcInput &rc) const
    {
        // Roll right is a positive rotation about x (y points left), nose
        // down a positive rotation about y, clockwise from above a negative
        // rotation about z (which points up).
        return {deflection(rc.roll) * m_rateRollPitchRadS,
                deflection(rc.pitch) * m_rateRollPitchRadS,
                -deflection(rc.yaw) * m_rateYawRadS};
    }

    float StickMapper::yawRateRadS(const RcInput &rc) const
    {
        return -deflection(rc.yaw) * m_rateYawRadS;
    }

    std::array<float, 3> StickMapper::desiredUpWorld(const RcInput &rc,
                                                     const Quaternion &attitude) const
    {
        // The lean the sticks ask for, in the heading frame: forward is the
        // body x axis with its tilt removed, right is minus the body y axis.
        // Written as the horizontal components of a unit-height vector, the
        // way brakeUpWorld() writes a braking lean, and clamped to the
        // maximum tilt as a whole so a diagonal stick leans no further than
        // a straight one.
        float forward = deflection(rc.pitch) * m_tanTiltMax;
        float right = deflection(rc.roll) * m_tanTiltMax;
        const float lean = std::sqrt(forward * forward + right * right);
        if (lean > m_tanTiltMax)
        {
            forward *= m_tanTiltMax / lean;
            right *= m_tanTiltMax / lean;
        }

        // Heading: the body x axis rotated into the world, projected on the
        // horizontal plane and normalized. Its cosine and sine rotate the
        // lean from the heading frame into the world frame.
        const Quaternion &q = attitude;
        const float noseX = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        const float noseY = 2.0f * (q.x * q.y + q.w * q.z);
        const float headingNorm = std::sqrt(noseX * noseX + noseY * noseY);
        float cosHeading = 1.0f;
        float sinHeading = 0.0f;
        if (headingNorm > MIN_HEADING_NORM)
        {
            cosHeading = noseX / headingNorm;
            sinHeading = noseY / headingNorm;
        }
        // Heading frame: x forward, y left, so right is -y.
        const float hx = forward;
        const float hy = -right;
        const float worldX = cosHeading * hx - sinHeading * hy;
        const float worldY = sinHeading * hx + cosHeading * hy;
        const float invNorm = 1.0f / std::sqrt(worldX * worldX + worldY * worldY + 1.0f);
        return {worldX * invNorm, worldY * invNorm, invNorm};
    }
} // namespace mark4
