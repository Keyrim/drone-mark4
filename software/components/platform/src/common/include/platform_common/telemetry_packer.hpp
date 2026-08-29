#pragma once

/// @file
/// @brief Conversion of the flight core state into the telemetry wire
///        format. An IO adapter of the composition layer: it reads only
///        public accessors, so flight-core never sees a wire layout.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flight_core/flight_core.hpp"
#include "flight_core/throw_detector.hpp"
#include "flight_core/types.hpp"
#include "protocol/header.hpp"
#include "protocol/telemetry.hpp"

namespace mark4
{
    /// @brief Packs one step into a telemetry datagram, ready to send.
    /// @param frame sensor frame that entered the core
    /// @param actuators actuator frame the core produced for it
    /// @param core flight core, source of the estimated state
    /// @param source stream identity of the sending process
    /// @param sequence per-sender counter, increments on every packet sent
    /// @return datagram bytes
    inline std::array<std::uint8_t, TELEMETRY_PACKET_SIZE> packTelemetry(
        const SensorFrame &frame,
        const ActuatorFrame &actuators,
        const FlightCore &core,
        StreamSource source,
        std::uint16_t sequence)
    {
        TelemetryPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::TELEMETRY);
        packet.sourceId = static_cast<std::uint8_t>(source);
        packet.sequence = sequence;
        packet.timestampUs = frame.timestampUs;
        packet.altitudeM = core.altitudeM();
        packet.baroAltitudeM = core.baroAltitudeM();
        packet.verticalVelocityMps = core.verticalVelocityMps();
        const ThrowDetector &detector = core.throwDetector();
        packet.throwState = static_cast<std::uint8_t>(detector.state());
        packet.throwCount = detector.throwCount();
        packet.releaseVelocityMps = detector.releaseVelocityMps();
        packet.apexTimestampUs = detector.apexTimestampUs();
        packet.apexAltitudeM = detector.apexAltitudeM();
        packet.flightPhase = static_cast<std::uint8_t>(core.flightPhase());

        std::array<std::uint8_t, TELEMETRY_PACKET_SIZE> wire{};
        std::memcpy(wire.data(), &packet, sizeof(packet));

        /* The std::array members sit at odd offsets in the packed struct:
           writing them through it would bind a reference to a misaligned
           address, so they go straight into the datagram bytes. */
        const Quaternion &q = core.attitude();
        const std::array<float, 4> quat = {q.w, q.x, q.y, q.z};
        const std::array<float, 3> bias = core.gyroBiasRadS();
        std::memcpy(wire.data() + offsetof(TelemetryPacket, gyroRadS),
                    frame.gyroRadS.data(),
                    sizeof(frame.gyroRadS));
        std::memcpy(
            wire.data() + offsetof(TelemetryPacket, attitudeQuat), quat.data(), sizeof(quat));
        std::memcpy(
            wire.data() + offsetof(TelemetryPacket, gyroBiasRadS), bias.data(), sizeof(bias));
        std::memcpy(wire.data() + offsetof(TelemetryPacket, motor),
                    actuators.motor.data(),
                    sizeof(actuators.motor));
        return wire;
    }
} // namespace mark4
