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
#include "platform_sim/udp_socket.hpp"
#include "protocol/envelope.hpp"

namespace
{
    /// Short enough that a broken expectation fails fast instead of hanging.
    constexpr std::uint32_t TEST_TIMEOUT_MS = 200U;

    constexpr std::uint64_t TEST_TIMESTAMP_US = 123456789U;
    constexpr std::array<float, 3> TEST_GYRO_RAD_S = {0.25f, -0.5f, 1.5f};
    constexpr std::array<float, 3> TEST_ACCEL_MPS2 = {0.0f, 0.0f, 9.80665f};
    constexpr float TEST_BARO_PA = 101325.0f;
    constexpr std::uint32_t TEST_RESET_COUNT = 3U;
    constexpr std::uint32_t TEST_LOCKSTEP_TIMEOUTS = 517U;

    /// Larger than any envelope, so a truncated read is never mistaken for a
    /// valid size.
    constexpr std::size_t WIRE_BUFFER_SIZE = mark4::MAX_ENVELOPE_SIZE + 64U;

    /// Encoded bytes of one datagram.
    struct Datagram
    {
        std::array<std::uint8_t, WIRE_BUFFER_SIZE> bytes{}; ///< the bytes
        std::size_t size = 0U;                              ///< how many are valid
    };

    /// @brief Encodes one envelope.
    /// @param envelope message to encode
    /// @return the datagram
    Datagram encode(const mark4_Envelope &envelope)
    {
        Datagram datagram;
        REQUIRE(mark4::encodeEnvelope(
            envelope, datagram.bytes.data(), datagram.bytes.size(), datagram.size));
        return datagram;
    }

    /// @brief Decodes one datagram.
    /// @param bytes datagram bytes
    /// @param size datagram size
    /// @return the envelope
    mark4_Envelope decode(const std::uint8_t *bytes, std::size_t size)
    {
        mark4_Envelope envelope;
        REQUIRE(mark4::decodeEnvelope(bytes, size, envelope));
        return envelope;
    }

    /// @brief Builds a sensor envelope, truth included.
    /// @param timestampUs simulated time carried by the message [us]
    /// @return the datagram
    Datagram makeSensorDatagram(std::uint64_t timestampUs = TEST_TIMESTAMP_US)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_sim_sensor_tag;
        mark4_SimSensor &sensor = envelope.body.sim_sensor;
        sensor.timestamp_us = timestampUs;
        std::memcpy(sensor.gyro_rad_s, TEST_GYRO_RAD_S.data(), sizeof(sensor.gyro_rad_s));
        std::memcpy(sensor.accel_mps2, TEST_ACCEL_MPS2.data(), sizeof(sensor.accel_mps2));
        sensor.baro_pa = TEST_BARO_PA;
        sensor.reset_count = TEST_RESET_COUNT;
        sensor.lockstep_timeouts = TEST_LOCKSTEP_TIMEOUTS;
        sensor.has_truth = true;
        sensor.truth.attitude_quat[0] = 1.0f;
        sensor.truth.position_m[2] = 1.5f;
        sensor.truth.velocity_mps[0] = -2.0f;
        return encode(envelope);
    }

    /// Plain UDP socket standing in for the simulator: sends sensor messages
    /// to a local port and reads the actuator messages sent back.
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

        /// @brief Sends one datagram to the given port.
        /// @param port destination port
        /// @param datagram bytes to send
        /// @return true when the whole datagram was sent
        [[nodiscard]] bool send(std::uint16_t port, const Datagram &datagram) const
        {
            return sendTo(port, datagram.bytes.data(), datagram.size);
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

        /// @brief Reads one datagram and decodes it.
        /// @return the envelope, which_body 0 when nothing came
        [[nodiscard]] mark4_Envelope receiveEnvelope() const
        {
            std::array<std::uint8_t, WIRE_BUFFER_SIZE> wire{};
            const std::size_t size = receive(wire.data(), wire.size());
            if (size == 0U)
            {
                return mark4_Envelope_init_zero;
            }
            return decode(wire.data(), size);
        }

      private:
        int m_fd; ///< sender socket, bound implicitly by the first send
    };

} // namespace

TEST_CASE("a sensor message is decoded into a sensor frame")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));
    REQUIRE(link.boundPort() != 0U);

    mark4::SensorSourceSim source(link);
    SimulatorStub simulator;

    REQUIRE(simulator.send(link.boundPort(), makeSensorDatagram()));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == TEST_TIMESTAMP_US);
    REQUIRE(frame.gyroRadS == TEST_GYRO_RAD_S);
    REQUIRE(frame.accelMps2 == TEST_ACCEL_MPS2);
    REQUIRE(frame.baroPa == TEST_BARO_PA);
    // The sensor message carries no RC: the decoded frame keeps the safe
    // defaults until the composition root grafts the tracked state onto it.
    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(source.resetCount() == TEST_RESET_COUNT);
    REQUIRE(source.lockstepTimeouts() == TEST_LOCKSTEP_TIMEOUTS);
    REQUIRE(source.plantRestarts() == 0U);
    // The plant's exact state travels with the sample, for the telemetry.
    REQUIRE(source.truth().attitude_quat[0] == 1.0f);
    REQUIRE(source.truth().position_m[2] == 1.5f);
    REQUIRE(source.truth().velocity_mps[0] == -2.0f);
}

TEST_CASE("malformed datagrams are skipped and the next valid one is delivered")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    SimulatorStub simulator;

    // Another body on the sensor port: an Rc message is not a sensor frame.
    mark4_Envelope other = mark4_Envelope_init_zero;
    other.which_body = mark4_Envelope_rc_tag;
    other.body.rc.throttle = 0.5f;
    REQUIRE(simulator.send(link.boundPort(), encode(other)));

    const Datagram valid = makeSensorDatagram();
    const std::array<std::uint8_t, 4> garbage = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    REQUIRE(simulator.sendTo(link.boundPort(), garbage.data(), garbage.size())); // not a message
    REQUIRE(simulator.send(link.boundPort(), valid));                            // good one

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == TEST_TIMESTAMP_US);
}

TEST_CASE("a stray datagram cannot redirect the motor replies")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;
    SimulatorStub intruder;

    // The simulator establishes itself with a valid message first.
    REQUIRE(simulator.send(link.boundPort(), makeSensorDatagram()));
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
    REQUIRE(simulator.receiveEnvelope().which_body == mark4_Envelope_sim_actuator_tag);
    REQUIRE(intruder.receive(wire.data(), wire.size()) == 0U);
}

TEST_CASE("a resent sensor message is answered again instead of stepped twice")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;

    const Datagram datagram = makeSensorDatagram();
    REQUIRE(simulator.send(link.boundPort(), datagram));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    mark4::ActuatorFrame actuators;
    actuators.timestampUs = frame.timestampUs;
    actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};
    sink.push(actuators);

    std::array<std::uint8_t, WIRE_BUFFER_SIZE> first{};
    const std::size_t firstSize = simulator.receive(first.data(), first.size());
    REQUIRE(firstSize > 0U);

    // The same tick arrives again: the simulator never got its reply.
    REQUIRE(simulator.send(link.boundPort(), datagram));
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);
    REQUIRE(source.duplicateFrameCount() == 1U);
    REQUIRE(sink.pushCount() == 1U); // the core was not stepped a second time

    // ...and the answer it missed went out again, byte for byte.
    std::array<std::uint8_t, WIRE_BUFFER_SIZE> repeat{};
    REQUIRE(simulator.receive(repeat.data(), repeat.size()) == firstSize);
    REQUIRE(std::memcmp(first.data(), repeat.data(), firstSize) == 0);
}

TEST_CASE("a restarted simulator is a new plant, not a resend")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    SimulatorStub simulator;

    REQUIRE(simulator.send(link.boundPort(), makeSensorDatagram(TEST_TIMESTAMP_US)));
    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    // A new plant: its simulated clock starts over, so an earlier timestamp
    // is a genuine new sample, not a resend, and the restart is counted.
    REQUIRE(simulator.send(link.boundPort(), makeSensorDatagram(1000U)));
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == 1000U);
    REQUIRE(source.plantRestarts() == 1U);
    REQUIRE(source.duplicateFrameCount() == 0U);
}

TEST_CASE("a scenario goes to the plant as its own message")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;

    mark4_SimScenario scenario = mark4_SimScenario_init_zero;
    scenario.sequence = 9U;
    scenario.kind = mark4_SimScenarioKind_THROW;
    scenario.seed = 0x0123456789ABCDEFULL;
    scenario.throw_delay_us = 2000000U;
    scenario.hash_window_us = 16000000U;
    scenario.swing_seconds = 0.375f;

    // No plant has spoken yet: the scenario waits for the first reply.
    sink.sendScenario(scenario);
    REQUIRE(simulator.receiveEnvelope().which_body == 0U);

    REQUIRE(simulator.send(link.boundPort(), makeSensorDatagram()));
    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    mark4::ActuatorFrame actuators;
    actuators.timestampUs = frame.timestampUs;
    actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};
    sink.push(actuators);

    // The pending scenario goes first, then the reply to the tick.
    const mark4_Envelope first = simulator.receiveEnvelope();
    REQUIRE(first.which_body == mark4_Envelope_sim_scenario_tag);
    REQUIRE(first.body.sim_scenario.sequence == 9U);
    REQUIRE(first.body.sim_scenario.kind == mark4_SimScenarioKind_THROW);
    REQUIRE(first.body.sim_scenario.seed == 0x0123456789ABCDEFULL);
    REQUIRE(first.body.sim_scenario.swing_seconds == 0.375f);
    REQUIRE(simulator.receiveEnvelope().which_body == mark4_Envelope_sim_actuator_tag);

    // With the plant known, a scenario goes out at once and only once.
    scenario.sequence = 10U;
    sink.sendScenario(scenario);
    REQUIRE(simulator.receiveEnvelope().body.sim_scenario.sequence == 10U);
    sink.push(actuators);
    REQUIRE(simulator.receiveEnvelope().which_body == mark4_Envelope_sim_actuator_tag);
    REQUIRE(simulator.receiveEnvelope().which_body == 0U);
}

TEST_CASE("an idle link ends the run")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);
}

TEST_CASE("pushed motors are sent back to the sensor sender")
{
    mark4::UdpSocket link;
    REQUIRE(link.open(0U, TEST_TIMEOUT_MS));

    mark4::SensorSourceSim source(link);
    mark4::MotorSinkSim sink(link);
    SimulatorStub simulator;

    REQUIRE(simulator.send(link.boundPort(), makeSensorDatagram()));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::FRAME);

    mark4::ActuatorFrame actuators;
    actuators.timestampUs = frame.timestampUs;
    actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};
    sink.push(actuators);

    REQUIRE(sink.pushCount() == 1U);
    REQUIRE(sink.last().motor == actuators.motor);

    const mark4_Envelope reply = simulator.receiveEnvelope();
    REQUIRE(reply.which_body == mark4_Envelope_sim_actuator_tag);
    // The reply echoes the sensor timestamp: the lockstep handshake.
    REQUIRE(reply.body.sim_actuator.echo_timestamp_us == TEST_TIMESTAMP_US);
    for (std::size_t motor = 0U; motor < actuators.motor.size(); ++motor)
    {
        REQUIRE(reply.body.sim_actuator.motor[motor] == actuators.motor[motor]);
    }
}
