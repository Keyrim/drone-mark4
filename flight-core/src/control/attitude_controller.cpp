#include "flight_core/attitude_controller.hpp"

namespace mark4
{
    std::array<float, 3> AttitudeController::rateSetpointRadS(const Quaternion &attitude) const
    {
        // World up axis seen from the body frame (same expression as the
        // estimator's gravity direction).
        const Quaternion &q = attitude;
        const float upX = 2.0f * (q.x * q.z - q.w * q.y);
        const float upY = 2.0f * (q.w * q.x + q.y * q.z);

        // Rotation bringing the body up axis onto the world up axis, as a
        // body frame rotation vector: e_z x up, whose magnitude is sin(tilt).
        return {m_kp * -upY, m_kp * upX, 0.0f};
    }
} // namespace mark4
