#pragma once

/// @file
/// @brief Motor mixer for the X quad layout.

#include <array>

namespace mark4
{
    /// @brief Distributes a collective command and three normalized torque
    ///        demands over the four motors of the X layout, then clamps each
    ///        command to [0, 1].
    ///
    /// Motor order matches the wire protocol and the simulator (4 in 1 ESC
    /// numbering). In the drone body frame (x forward, y left, z up):
    /// motor 0 rear right, 1 front right, 2 rear left, 3 front left; the
    /// diagonal pairs (0, 3) and (1, 2) spin in opposite directions, so their
    /// reaction torques yaw the airframe in opposite ways.
    ///
    /// @param collective common motor command, the vertical loop output [0, 1]
    /// @param torqueCmd normalized torque demands about body x, y, z [-1, 1]
    /// @return four motor commands in [0, 1]
    std::array<float, 4> mixMotors(float collective, const std::array<float, 3> &torqueCmd);
} // namespace mark4
