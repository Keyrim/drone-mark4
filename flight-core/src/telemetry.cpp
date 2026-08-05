#include "flight_core/telemetry.hpp"

#include <cstddef>
#include <cstring>

#include "protocol/version.hpp"

namespace mark4
{
    std::array<std::uint8_t, TELEMETRY_PACKET_SIZE> packTelemetry(const SensorFrame &frame,
                                                                  const ActuatorFrame &actuators,
                                                                  const FlightCore &core)
    {
        TelemetryPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.timestampUs = frame.timestampUs;
        packet.altitudeM = core.altitudeM();
        packet.verticalVelocityMps = core.verticalVelocityMps();
        const ThrowDetector &detector = core.throwDetector();
        packet.throwState = static_cast<std::uint8_t>(detector.state());
        packet.throwCount = detector.throwCount();
        packet.releaseVelocityMps = detector.releaseVelocityMps();
        packet.apexTimestampUs = detector.apexTimestampUs();
        packet.apexAltitudeM = detector.apexAltitudeM();

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
