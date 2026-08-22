#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/types.hpp"
#include "platform_sim/command_receiver_sim.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/udp_link.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"
#include "protocol/sim_link.hpp"

namespace
{
    /// Short enough that a broken expectation fails fast instead of hanging.
    constexpr std::uint32_t TEST_TIMEOUT_MS = 200U;

    constexpr std::uint64_t TEST_TIMESTAMP_US = 123456789U;
    constexpr std::array<float, 3> TEST_GYRO_RAD_S = {0.25f, -0.5f, 1.5f};
    constexpr std::array<float, 3> TEST_ACCEL_MPS2 = {0.0f, 0.0f, 9.80665f};
    constexpr float TEST_BARO_PA = 101325.0f;
    constexpr float TEST_THROTTLE = 0.75f;
    constexpr std::uint8_t TEST_RESET_COUNT = 3U;
    constexpr std::uint32_t TEST_SESSION_ID = 0xC0FFEE01U;
    constexpr std::uint16_t TEST_LOCKSTEP_TIMEOUTS = 517U;

    /// Larger than any packet of the protocol, so a truncated read is never
    /// mistaken for a valid size.
    constexpr std::size_t WIRE_BUFFER_SIZE = 128U;

    /// Datagram bytes of a sensor packet, all fields distinctive.
    using SensorDatagram = std::array<std::uint8_t, mark4::SIM_SENSOR_PACKET_SIZE>;

    /// @brief Builds a sensor packet datagram.
    /// @param timestampUs simulated time carried by the packet [us]
    /// @param sessionId simulator session the packet claims to come from
    /// @return wire bytes of a valid sensor packet
    SensorDatagram makeSensorDatagram(std::uint64_t timestampUs = TEST_TIMESTAMP_US,
                                      std::uint32_t sessionId = TEST_SESSION_ID)
    {
        mark4::SimSensorPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_SENSOR);
        packet.timestampUs = timestampUs;
        packet.baroPa = TEST_BARO_PA;
        packet.resetCount = TEST_RESET_COUNT;
        packet.sessionId = sessionId;
        packet.lockstepTimeouts = TEST_LOCKSTEP_TIMEOUTS;

        SensorDatagram wire{};
        std::memcpy(wire.data(), &packet, sizeof(packet));
        // The array fields are misaligned inside the packed struct: they are
        // written straight into the datagram bytes instead.
        std::memcpy(wire.data() + offsetof(mark4::SimSensorPacket, gyroRadS),
                    TEST_GYRO_RAD_S.data(),
                    sizeof(TEST_GYRO_RAD_S));
        std::memcpy(wire.data() + offsetof(mark4::SimSensorPacket, accelMps2),
                    TEST_ACCEL_MPS2.data(),
                    sizeof(TEST_ACCEL_MPS2));
        return wire;
    }

    /// Plain UDP socket standing in for the simulator: sends sensor packets to
    /// a local port and reads the actuator packets sent back.
    class SimulatorStub
    {
      public:
        SimulatorStub()
            : m_fd(::socket(AF_INET, SOCK_DGRAM, 0))
        {
            REQUIRE(m_fd >= 0);
            timeval timeout{};
            timeout.tv_usec = static_cast<suseconds_t>(TEST_TIMEOUT_MS) * 1000;
            REQUIRE(::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        }

        ~SimulatorStub()
        {
            static_cast<void>(::close(m_fd));
        }

        SimulatorStub(const SimulatorStub &) = delete;
        SimulatorStub &operator=(const SimulatorStub &) = delete;
        SimulatorStub(SimulatorStub &&) = delete;
        SimulatorStub &operator=(SimulatorStub &&) = delete;

        /// @brief Sends raw bytes to 127.0.0.1 on the given port.
        /// @param port destination port
        /// @param data bytes to send
        /// @param size number of bytes to send
        /// @return true when the whole datagram was sent
        bool sendTo(std::uint16_t port, const void *data, std::size_t size) const
        {
            if (m_fd < 0)
            {
                return false;
            }

            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(port);
            target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const ssize_t sent = ::sendto(
                m_fd, data, size, 0, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
            return sent >= 0 && static_cast<std::size_t>(sent) == size;
        }

        /// @brief Reads one datagram, giving up after the receive timeout.
        /// @param[out] bufferOut receives the datagram bytes
        /// @param capacity size of bufferOut
        /// @return number of bytes received, 0 on timeout
        std::size_t receive(void *bufferOut, std::size_t capacity) const
        {
            if (m_fd < 0)
            {
                return 0U;
            }

            const ssize_t received = ::recv(m_fd, bufferOut, capacity, 0);
            return received > 0 ? static_cast<std::size_t>(received) : 0U;
        }

      private:
        int m_fd; ///< sender socket, bound implicitly by the first send
    };

    /// @brief Polls a non-blocking receiver until a datagram shows up or the
    ///        test timeout expires. The kernel does not necessarily hand a
    ///        loopback datagram over before the sender's sendto() returns,
    ///        so a single poll may legitimately come back empty - the flight
    ///        loop polls once per frame for the same reason.
    /// @param receiver receiver under test
    /// @param[out] bufferOut receives the datagram bytes
    /// @param capacity size of bufferOut
    /// @return datagram size, 0 when none arrived before the timeout
    std::size_t pollUntilPacket(mark4::CommandReceiverSim &receiver,
                                std::uint8_t *bufferOut,
                                std::size_t capacity)
    {
        constexpr std::uint32_t POLL_INTERVAL_US = 1000U;
        constexpr std::uint32_t POLL_ATTEMPTS = TEST_TIMEOUT_MS;
        for (std::uint32_t attempt = 0U; attempt < POLL_ATTEMPTS; ++attempt)
        {
            const std::size_t received = receiver.poll(bufferOut, capacity);
            if (received > 0U)
            {
                return received;
            }
            ::usleep(POLL_INTERVAL_US);
        }
        return 0U;
    }
} // namespace

TEST_CASE("a sensor packet is decoded into a sensor frame")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));
    REQUIRE(link.boundPort() != 0U);

    mark4::SensorSourceSim source(link);
    SimulatorStub simulator;

    const SensorDatagram datagram = makeSensorDatagram();
    REQUIRE(simulator.sendTo(link.boundPort(), datagram.data(), datagram.size()));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == TEST_TIMESTAMP_US);
    REQUIRE(frame.gyroRadS == TEST_GYRO_RAD_S);
    REQUIRE(frame.accelMps2 == TEST_ACCEL_MPS2);
    REQUIRE(frame.baroPa == TEST_BARO_PA);
    // The sensor packet carries no RC: the decoded frame keeps the safe
    // defaults until the composition root grafts the tracked state onto it.
    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(source.resetCount() == TEST_RESET_COUNT);
    REQUIRE(source.sessionId() == TEST_SESSION_ID);
    REQUIRE(source.lockstepTimeouts() == TEST_LOCKSTEP_TIMEOUTS);
}

TEST_CASE("malformed datagrams are skipped and the next valid one is delivered")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    SimulatorStub simulator;

    SensorDatagram wrongVersion = makeSensorDatagram();
    wrongVersion[offsetof(mark4::SimSensorPacket, version)] =
        static_cast<std::uint8_t>(mark4::PROTOCOL_VERSION + 1U);
    REQUIRE(simulator.sendTo(link.boundPort(), wrongVersion.data(), wrongVersion.size()));

    SensorDatagram wrongType = makeSensorDatagram();
    wrongType[offsetof(mark4::SimSensorPacket, type)] =
        static_cast<std::uint8_t>(mark4::PacketType::SIM_ACTUATOR);
    REQUIRE(simulator.sendTo(link.boundPort(), wrongType.data(), wrongType.size()));

    const SensorDatagram valid = makeSensorDatagram();
    REQUIRE(simulator.sendTo(link.boundPort(), valid.data(), 8U));           // too short
    REQUIRE(simulator.sendTo(link.boundPort(), valid.data(), valid.size())); // good one

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == TEST_TIMESTAMP_US);
}

TEST_CASE("a stray datagram cannot redirect the motor replies")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;
    SimulatorStub intruder;

    // The simulator establishes itself with a valid packet first.
    const SensorDatagram datagram = makeSensorDatagram();
    REQUIRE(simulator.sendTo(link.boundPort(), datagram.data(), datagram.size()));
    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    // A stray datagram lands on the port; the source rejects it (and times
    // out waiting for a valid one), but the sender must not be latched.
    const std::array<std::uint8_t, 4> garbage = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    REQUIRE(intruder.sendTo(link.boundPort(), garbage.data(), garbage.size()));
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);

    // The motor reply still reaches the simulator, not the intruder.
    mark4::ActuatorFrame actuators;
    actuators.timestampUs = frame.timestampUs;
    actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};
    sink.push(actuators);

    std::array<std::uint8_t, WIRE_BUFFER_SIZE> wire{};
    REQUIRE(simulator.receive(wire.data(), wire.size()) == mark4::SIM_ACTUATOR_PACKET_SIZE);
    REQUIRE(intruder.receive(wire.data(), wire.size()) == 0U);
}

TEST_CASE("a resent sensor packet is answered again instead of stepped twice")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;

    const SensorDatagram datagram = makeSensorDatagram();
    REQUIRE(simulator.sendTo(link.boundPort(), datagram.data(), datagram.size()));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    mark4::ActuatorFrame actuators;
    actuators.timestampUs = frame.timestampUs;
    actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};
    sink.push(actuators);

    std::array<std::uint8_t, WIRE_BUFFER_SIZE> first{};
    const std::size_t firstSize = simulator.receive(first.data(), first.size());
    REQUIRE(firstSize == mark4::SIM_ACTUATOR_PACKET_SIZE);

    // The same tick arrives again: the simulator never got its reply.
    REQUIRE(simulator.sendTo(link.boundPort(), datagram.data(), datagram.size()));
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);
    REQUIRE(source.duplicateFrameCount() == 1U);
    REQUIRE(sink.pushCount() == 1U); // the core was not stepped a second time

    // ...and the answer it missed went out again, byte for byte.
    std::array<std::uint8_t, WIRE_BUFFER_SIZE> repeat{};
    REQUIRE(simulator.receive(repeat.data(), repeat.size()) == firstSize);
    REQUIRE(std::memcmp(first.data(), repeat.data(), firstSize) == 0);
}

TEST_CASE("a restarted simulator may replay a timestamp already seen")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    SimulatorStub simulator;

    const SensorDatagram late = makeSensorDatagram(TEST_TIMESTAMP_US);
    REQUIRE(simulator.sendTo(link.boundPort(), late.data(), late.size()));
    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    // A new plant: its simulated clock starts over, so an earlier timestamp -
    // or the very same one - is a genuine new sample, not a resend.
    const SensorDatagram fresh = makeSensorDatagram(1000U, TEST_SESSION_ID + 1U);
    REQUIRE(simulator.sendTo(link.boundPort(), fresh.data(), fresh.size()));
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == 1000U);
    REQUIRE(source.sessionId() == TEST_SESSION_ID + 1U);
    REQUIRE(source.duplicateFrameCount() == 0U);
}

TEST_CASE("the attached scenario rides in every reply, byte for byte")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;

    mark4::SimScenario scenario{};
    scenario.sequence = 9U;
    scenario.scenario = mark4::SIM_SCENARIO_THROW;
    scenario.seed = 0x0123456789ABCDEFULL;
    scenario.throwDelayUs = 2000000U;
    scenario.hashWindowUs = 16000000U;
    scenario.swingSeconds = 0.375f;
    std::array<std::uint8_t, mark4::SIM_SCENARIO_SIZE> expected{};
    std::memcpy(expected.data(), &scenario, sizeof(scenario));
    sink.attachScenario(scenario);

    const SensorDatagram datagram = makeSensorDatagram();
    REQUIRE(simulator.sendTo(link.boundPort(), datagram.data(), datagram.size()));
    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    mark4::ActuatorFrame actuators;
    actuators.timestampUs = frame.timestampUs;
    actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};

    // Repeated on every reply: the plant dedups on the sequence byte, so a
    // block cannot be missed by a peer that missed one datagram.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        sink.push(actuators);
        std::array<std::uint8_t, WIRE_BUFFER_SIZE> wire{};
        REQUIRE(simulator.receive(wire.data(), wire.size()) == mark4::SIM_ACTUATOR_PACKET_SIZE);
        REQUIRE(std::memcmp(wire.data() + offsetof(mark4::SimActuatorPacket, scenario),
                            expected.data(),
                            expected.size()) == 0);
    }
}

TEST_CASE("an idle link ends the run")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);
}

TEST_CASE("pushed motors are sent back to the sensor sender")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;

    const SensorDatagram datagram = makeSensorDatagram();
    REQUIRE(simulator.sendTo(link.boundPort(), datagram.data(), datagram.size()));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    mark4::ActuatorFrame actuators;
    actuators.timestampUs = frame.timestampUs;
    actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};
    sink.push(actuators);

    REQUIRE(sink.pushCount() == 1U);
    REQUIRE(sink.last().motor == actuators.motor);

    std::array<std::uint8_t, WIRE_BUFFER_SIZE> wire{};
    REQUIRE(simulator.receive(wire.data(), wire.size()) == mark4::SIM_ACTUATOR_PACKET_SIZE);
    REQUIRE(wire[offsetof(mark4::SimActuatorPacket, version)] == mark4::PROTOCOL_VERSION);
    REQUIRE(wire[offsetof(mark4::SimActuatorPacket, type)] ==
            static_cast<std::uint8_t>(mark4::PacketType::SIM_ACTUATOR));

    // The reply echoes the sensor timestamp: the lockstep handshake.
    std::uint64_t echo = 0U;
    std::memcpy(
        &echo, wire.data() + offsetof(mark4::SimActuatorPacket, echoTimestampUs), sizeof(echo));
    REQUIRE(echo == TEST_TIMESTAMP_US);

    std::array<float, 4> motor{};
    std::memcpy(
        motor.data(), wire.data() + offsetof(mark4::SimActuatorPacket, motor), sizeof(motor));
    REQUIRE(motor == actuators.motor);
}

TEST_CASE("an rc command datagram comes out of poll")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));
    REQUIRE(link.boundPort() != 0U);

    mark4::CommandReceiverSim receiver(link);
    SimulatorStub pilot;

    mark4::RcCommandPacket packet{};
    packet.version = mark4::PROTOCOL_VERSION;
    packet.type = static_cast<std::uint8_t>(mark4::PacketType::RC_COMMAND);
    packet.killSwitch = 0U;
    packet.armSwitch = 1U;
    packet.mode = mark4::RC_MODE_MANUAL;
    packet.throttle = TEST_THROTTLE;

    std::array<std::uint8_t, mark4::RC_COMMAND_PACKET_SIZE> sent{};
    std::memcpy(sent.data(), &packet, sizeof(packet));
    REQUIRE(pilot.sendTo(link.boundPort(), sent.data(), sent.size()));

    std::array<std::uint8_t, WIRE_BUFFER_SIZE> wire{};
    REQUIRE(pollUntilPacket(receiver, wire.data(), wire.size()) == mark4::RC_COMMAND_PACKET_SIZE);
    REQUIRE(std::memcmp(wire.data(), sent.data(), sent.size()) == 0);
    REQUIRE(receiver.packetsReceived() == 1U);
}

TEST_CASE("poll returns 0 immediately when nothing is pending")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::CommandReceiverSim receiver(link);

    // The link opens with a receive timeout: only a non-blocking read comes
    // back at once, which is what the flight loop needs.
    std::array<std::uint8_t, WIRE_BUFFER_SIZE> wire{};
    REQUIRE(receiver.poll(wire.data(), wire.size()) == 0U);
    REQUIRE(receiver.packetsReceived() == 0U);
}
