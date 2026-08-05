#pragma once

/// @file
/// @brief Conversion of the internal state into the telemetry wire format.

#include <array>
#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "protocol/telemetry.hpp"

namespace mark4
{
    /// @brief Packs one step into a telemetry datagram, ready to send.
    /// @param frame sensor frame that entered the core
    /// @param actuators actuator frame the core produced for it
    /// @param core flight core, source of the estimated state
    /// @return datagram bytes
    std::array<std::uint8_t, TELEMETRY_PACKET_SIZE> packTelemetry(const SensorFrame &frame,
                                                                  const ActuatorFrame &actuators,
                                                                  const FlightCore &core);
} // namespace mark4
