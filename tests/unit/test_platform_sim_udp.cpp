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
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/udp_link.hpp"
#include "protocol/sim_link.hpp"
#include "protocol/version.hpp"

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

    /// Datagram bytes of a sensor packet, all fields distinctive.
    using SensorDatagram = std::array<std::uint8_t, mark4::SIM_SENSOR_PACKET_SIZE>;

    /// @return wire bytes of a valid sensor packet
    SensorDatagram makeSensorDatagram()
    {
        mark4::SimSensorPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.timestampUs = TEST_TIMESTAMP_US;
        packet.baroPa = TEST_BARO_PA;
        packet.killSwitch = 0U;
        packet.throttle = TEST_THROTTLE;
        packet.armSwitch = 1U;
        packet.resetCount = TEST_RESET_COUNT;

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
    REQUIRE(frame.rc.killSwitch == false);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
    REQUIRE(frame.rc.armSwitch == true);
    REQUIRE(source.resetCount() == TEST_RESET_COUNT);
}

TEST_CASE("an engaged kill switch reaches the frame")
{
    mark4::UdpLink link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    SimulatorStub simulator;

    SensorDatagram datagram = makeSensorDatagram();
    datagram[offsetof(mark4::SimSensorPacket, killSwitch)] = 1U;
    REQUIRE(simulator.sendTo(link.boundPort(), datagram.data(), datagram.size()));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.rc.killSwitch == true);
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

    const SensorDatagram valid = makeSensorDatagram();
    REQUIRE(simulator.sendTo(link.boundPort(), valid.data(), 8U));           // too short
    REQUIRE(simulator.sendTo(link.boundPort(), valid.data(), valid.size())); // good one

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == TEST_TIMESTAMP_US);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
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

    std::array<std::uint8_t, 64> wire{};
    REQUIRE(simulator.receive(wire.data(), wire.size()) == mark4::SIM_ACTUATOR_PACKET_SIZE);
    REQUIRE(wire[offsetof(mark4::SimActuatorPacket, version)] == mark4::PROTOCOL_VERSION);

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
