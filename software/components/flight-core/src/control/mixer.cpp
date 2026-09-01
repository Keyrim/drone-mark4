#include "flight_core/mixer.hpp"

namespace mark4
{
    namespace
    {
        /// @return value clamped to [0, 1]
        float clampCommand(float value)
        {
            if (value < 0.0f)
            {
                return 0.0f;
            }
            return value > 1.0f ? 1.0f : value;
        }
    } // namespace

    std::array<float, 4> mixMotors(float collective, const std::array<float, 3> &torqueCmd)
    {
        const float roll = torqueCmd[0];  // + = more thrust on the left motors
        const float pitch = torqueCmd[1]; // + = more thrust on the rear motors
        const float yaw = torqueCmd[2];   // + = more thrust on the (1, 2) pair

        return {
            clampCommand(collective - roll + pitch - yaw), // 0: rear right
            clampCommand(collective - roll - pitch + yaw), // 1: front right
            clampCommand(collective + roll + pitch + yaw), // 2: rear left
            clampCommand(collective + roll - pitch - yaw), // 3: front left
        };
    }
} // namespace mark4
