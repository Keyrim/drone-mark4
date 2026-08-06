#include "flight_core/attitude_controller.hpp"

namespace mark4
{
    std::array<float, 3> AttitudeController::rateSetpointRadS(
        const Quaternion &attitude, const std::array<float, 3> &desiredUpWorld) const
    {
        // Desired up axis seen from the body frame: the world vector rotated
        // by the inverse (world-to-body) attitude.
        const Quaternion &q = attitude;
        const float wx = desiredUpWorld[0];
        const float wy = desiredUpWorld[1];
        const float wz = desiredUpWorld[2];
        const float upX = (1.0f - 2.0f * (q.y * q.y + q.z * q.z)) * wx +
                          2.0f * (q.x * q.y + q.w * q.z) * wy + 2.0f * (q.x * q.z - q.w * q.y) * wz;
        const float upY = 2.0f * (q.x * q.y - q.w * q.z) * wx +
                          (1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * wy +
                          2.0f * (q.y * q.z + q.w * q.x) * wz;

        // Rotation bringing the body up axis onto the desired axis, as a
        // body frame rotation vector: e_z x up, whose magnitude is sin(tilt).
        return {m_kp * -upY, m_kp * upX, 0.0f};
    }
} // namespace mark4
