/// @file
/// @brief The sim link over the transport: a fake plant node on a private
///        discovery port unicasts SimSensor frames to the drone_sim
///        composition (PlantLink + SensorSourceSim + MotorSinkSim) and
///        reads what comes back, the way Godot does.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <netinet/in.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/types.hpp"
#include "platform_common/command_receiver_transport.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/plant_link.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "protocol/envelope.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

namespace
{
    /// Short enough that a broken expectation fails fast instead of hanging.
    constexpr std::uint32_t TEST_TIMEOUT_MS = 200U;

    constexpr std::uint32_t DRONE_NODE = 0xD0000001U;
    constexpr std::uint32_t PLANT_NODE = 0xB1A00001U;
    constexpr std::uint32_t OTHER_PLANT_NODE = 0xB1A00002U;

    constexpr std::uint64_t TEST_TIMESTAMP_US = 123456789U;
    constexpr std::array<float, 3> TEST_GYRO_RAD_S = {0.25f, -0.5f, 1.5f};
    constexpr std::array<float, 3> TEST_ACCEL_MPS2 = {0.0f, 0.0f, 9.80665f};
    constexpr float TEST_BARO_PA = 101325.0f;
    constexpr std::uint32_t TEST_RESET_COUNT = 3U;
    constexpr std::uint32_t TEST_LOCKSTEP_TIMEOUTS = 517U;

    /// @return a free UDP port of this host, for a private discovery port
    std::uint16_t pickFreePort()
    {
        mark4::UdpLink probe(0U);
        REQUIRE(probe.init());
        return probe.dataPort();
    }

    /// @brief Builds a sensor envelope, truth included.
    /// @param timestampUs simulated time carried by the message [us]
    /// @return the envelope
    mark4_Envelope makeSensor(std::uint64_t timestampUs = TEST_TIMESTAMP_US)
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
        return envelope;
    }

    /// One frame read off the fake plant's link.
    struct Received
    {
        mark4::FrameHeader header;                              ///< transport header
        std::array<std::uint8_t, mark4::MAX_PAYLOAD> payload{}; ///< payload bytes
        std::size_t size = 0U;                                  ///< payload size, 0 = nothing
    };

    /// The plant side, at the link level: frames built by hand and sent to
    /// the drone's data port, replies read raw, so the test sees the header
    /// the drone puts on the wire.
    class FakePlant
    {
      public:
        FakePlant(std::uint32_t nodeId, std::uint16_t discoveryPort, std::uint16_t dronePort)
            : m_nodeId(nodeId),
              m_link(discoveryPort)
        {
            REQUIRE(m_link.init());
            m_drone.host = INADDR_LOOPBACK;
            m_drone.port = dronePort;
        }

        /// @brief Frames one envelope for the drone node and sends it.
        void send(const mark4_Envelope &envelope, std::uint32_t dst = DRONE_NODE)
        {
            std::array<std::uint8_t, mark4::MAX_FRAME_SIZE> frame{};
            mark4::FrameHeader header;
            header.src = m_nodeId;
            header.dst = dst;
            header.seq = m_seq;
            header.hops = mark4::Transport::INITIAL_HOPS;
            ++m_seq;
            mark4::encodeFrameHeader(header, frame.data());
            std::size_t size = 0U;
            REQUIRE(mark4::encodeEnvelope(envelope,
                                          frame.data() + mark4::FRAME_HEADER_SIZE,
                                          frame.size() - mark4::FRAME_HEADER_SIZE,
                                          size));
            REQUIRE(m_link.send(frame.data(), mark4::FRAME_HEADER_SIZE + size, m_drone));
        }

        /// @brief Waits for one frame, up to the test timeout.
        /// @return what came, size 0 when nothing did
        Received receive()
        {
            Received received;
            std::array<std::uint8_t, mark4::MAX_FRAME_SIZE> frame{};
            for (std::uint32_t waited = 0U; waited < TEST_TIMEOUT_MS; ++waited)
            {
                mark4::LinkAddress from;
                const std::size_t size = m_link.receive(frame.data(), frame.size(), from);
                if (size > 0U)
                {
                    REQUIRE(mark4::decodeFrameHeader(frame.data(), size, received.header));
                    received.size = size - mark4::FRAME_HEADER_SIZE;
                    std::memcpy(received.payload.data(),
                                frame.data() + mark4::FRAME_HEADER_SIZE,
                                received.size);
                    return received;
                }
                ::usleep(1000);
            }
            return received;
        }

        /// @brief Waits for one frame and decodes its envelope.
        /// @return the envelope, which_body 0 when nothing came
        mark4_Envelope receiveEnvelope()
        {
            const Received received = receive();
            if (received.size == 0U)
            {
                return mark4_Envelope_init_zero;
            }
            mark4_Envelope envelope;
            REQUIRE(mark4::decodeEnvelope(received.payload.data(), received.size, envelope));
            return envelope;
        }

      private:
        std::uint32_t m_nodeId;     ///< this plant's node id
        std::uint16_t m_seq = 0U;   ///< next frame sequence
        mark4::UdpLink m_link;      ///< the plant's link
        mark4::LinkAddress m_drone; ///< the drone's data socket
    };

    /// The drone_sim side of the sim link, composed as the app does.
    struct DroneSide
    {
        explicit DroneSide(std::uint16_t discoveryPort)
            : udpLink(discoveryPort)
        {
            REQUIRE(udpLink.init());
            REQUIRE(transport.addLink(udpLink));
            REQUIRE(transport.init());
        }

        mark4::ClockSim clock;
        mark4::UdpLink udpLink;
        mark4::Transport transport{DRONE_NODE};
        mark4::CommandReceiverTransport commands;
        mark4::PlantLink link{transport, udpLink, clock, commands, TEST_TIMEOUT_MS};
        mark4::SensorSourceSim source{link};
        mark4::MotorSinkSim sink{link};
    };

    mark4::ActuatorFrame makeActuators(std::uint64_t timestampUs)
    {
        mark4::ActuatorFrame actuators;
        actuators.timestampUs = timestampUs;
        actuators.motor = {0.1f, 0.2f, 0.3f, 0.4f};
        return actuators;
    }
} // namespace

TEST_CASE("a sensor frame from the plant node is decoded into a sensor frame")
{
    const std::uint16_t port = pickFreePort();
    DroneSide drone(port);
    FakePlant plant(PLANT_NODE, port, drone.udpLink.dataPort());

    plant.send(makeSensor());

    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == TEST_TIMESTAMP_US);
    REQUIRE(frame.gyroRadS == TEST_GYRO_RAD_S);
    REQUIRE(frame.accelMps2 == TEST_ACCEL_MPS2);
    REQUIRE(frame.baroPa == TEST_BARO_PA);
    // The sensor message carries no RC: the decoded frame keeps the safe
    // defaults until the composition root grafts the tracked state onto it.
    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(drone.source.resetCount() == TEST_RESET_COUNT);
    REQUIRE(drone.source.lockstepTimeouts() == TEST_LOCKSTEP_TIMEOUTS);
    REQUIRE(drone.source.plantRestarts() == 0U);
    // The plant's exact state travels with the sample, for the telemetry.
    REQUIRE(drone.source.truth().attitude_quat[0] == 1.0f);
    REQUIRE(drone.source.truth().position_m[2] == 1.5f);
    REQUIRE(drone.source.truth().velocity_mps[0] == -2.0f);
    // The sender became THE plant, and the transport learnt it.
    REQUIRE(drone.link.plant() == PLANT_NODE);
    REQUIRE(drone.transport.isAlive(PLANT_NODE));
}

TEST_CASE("payloads that are not sensor frames go to the command ring")
{
    const std::uint16_t port = pickFreePort();
    DroneSide drone(port);
    FakePlant plant(PLANT_NODE, port, drone.udpLink.dataPort());

    // An Rc message ahead of the sensor frame: a command, queued for the
    // composition root, never mistaken for a sample.
    mark4_Envelope other = mark4_Envelope_init_zero;
    other.which_body = mark4_Envelope_rc_tag;
    other.body.rc.throttle = 0.5f;
    plant.send(other);
    plant.send(makeSensor());

    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == TEST_TIMESTAMP_US);

    std::array<std::uint8_t, mark4::MAX_PAYLOAD> payload{};
    const std::size_t size = drone.commands.poll(payload.data(), payload.size());
    REQUIRE(size > 0U);
    mark4_Envelope queued;
    REQUIRE(mark4::decodeEnvelope(payload.data(), size, queued));
    REQUIRE(queued.which_body == mark4_Envelope_rc_tag);
    REQUIRE(queued.body.rc.throttle == 0.5f);
    REQUIRE(drone.commands.poll(payload.data(), payload.size()) == 0U);
}

TEST_CASE("a second plant cannot take over the sim link while the first is alive")
{
    const std::uint16_t port = pickFreePort();
    DroneSide drone(port);
    FakePlant plant(PLANT_NODE, port, drone.udpLink.dataPort());
    FakePlant intruder(OTHER_PLANT_NODE, port, drone.udpLink.dataPort());

    plant.send(makeSensor());
    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);

    // Another node's sensor frames are counted and ignored: the wait times
    // out instead of stepping on somebody else's world.
    intruder.send(makeSensor(TEST_TIMESTAMP_US + 2000U));
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);
    REQUIRE(drone.source.foreignFrameCount() == 1U);
    REQUIRE(drone.link.plant() == PLANT_NODE);

    // The motor reply still reaches the plant, not the intruder.
    drone.sink.push(makeActuators(frame.timestampUs));
    const Received reply = plant.receive();
    REQUIRE(reply.size > 0U);
    CHECK(reply.header.src == DRONE_NODE);
    CHECK(reply.header.dst == PLANT_NODE);
    REQUIRE(intruder.receive().size == 0U);
}

TEST_CASE("a resent sensor frame is answered again instead of stepped twice")
{
    const std::uint16_t port = pickFreePort();
    DroneSide drone(port);
    FakePlant plant(PLANT_NODE, port, drone.udpLink.dataPort());

    const mark4_Envelope sensor = makeSensor();
    plant.send(sensor);
    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);
    drone.sink.push(makeActuators(frame.timestampUs));
    const Received first = plant.receive();
    REQUIRE(first.size > 0U);

    // The same tick arrives again, as a new frame of the plant (the
    // transport would drop an exact repeat of the sequence as a duplicate):
    // the plant never got its reply.
    plant.send(sensor);
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);
    REQUIRE(drone.source.duplicateFrameCount() == 1U);
    REQUIRE(drone.sink.pushCount() == 1U); // the core was not stepped a second time

    // ...and the answer it missed went out again, the same envelope.
    const Received repeat = plant.receive();
    REQUIRE(repeat.size == first.size);
    REQUIRE(std::memcmp(first.payload.data(), repeat.payload.data(), first.size) == 0);
}

TEST_CASE("a restarted plant is a new plant, not a resend")
{
    const std::uint16_t port = pickFreePort();
    DroneSide drone(port);
    FakePlant plant(PLANT_NODE, port, drone.udpLink.dataPort());

    plant.send(makeSensor(TEST_TIMESTAMP_US));
    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);

    // The same node, its simulated clock starting over: a genuine new
    // sample, and the restart is counted.
    plant.send(makeSensor(1000U));
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);
    REQUIRE(frame.timestampUs == 1000U);
    REQUIRE(drone.source.plantRestarts() == 1U);
    REQUIRE(drone.source.duplicateFrameCount() == 0U);
}

TEST_CASE("a scenario goes to the plant as its own frame")
{
    const std::uint16_t port = pickFreePort();
    DroneSide drone(port);
    FakePlant plant(PLANT_NODE, port, drone.udpLink.dataPort());

    mark4_SimScenario scenario = mark4_SimScenario_init_zero;
    scenario.sequence = 9U;
    scenario.kind = mark4_SimScenarioKind_THROW;
    scenario.seed = 0x0123456789ABCDEFULL;
    scenario.throw_delay_us = 2000000U;
    scenario.hash_window_us = 16000000U;
    scenario.swing_seconds = 0.375f;

    // No plant has spoken yet: the scenario waits for the first reply.
    drone.sink.sendScenario(scenario);
    REQUIRE(plant.receiveEnvelope().which_body == 0U);

    plant.send(makeSensor());
    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);
    drone.sink.push(makeActuators(frame.timestampUs));

    // The pending scenario goes first, then the reply to the tick.
    const mark4_Envelope first = plant.receiveEnvelope();
    REQUIRE(first.which_body == mark4_Envelope_sim_scenario_tag);
    REQUIRE(first.body.sim_scenario.sequence == 9U);
    REQUIRE(first.body.sim_scenario.kind == mark4_SimScenarioKind_THROW);
    REQUIRE(first.body.sim_scenario.seed == 0x0123456789ABCDEFULL);
    REQUIRE(first.body.sim_scenario.swing_seconds == 0.375f);
    REQUIRE(plant.receiveEnvelope().which_body == mark4_Envelope_sim_actuator_tag);

    // With the plant known, a scenario goes out at once and only once.
    scenario.sequence = 10U;
    drone.sink.sendScenario(scenario);
    REQUIRE(plant.receiveEnvelope().body.sim_scenario.sequence == 10U);
    drone.sink.push(makeActuators(frame.timestampUs));
    REQUIRE(plant.receiveEnvelope().which_body == mark4_Envelope_sim_actuator_tag);
    REQUIRE(plant.receiveEnvelope().which_body == 0U);
}

TEST_CASE("an idle sim link times out")
{
    DroneSide drone(pickFreePort());
    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::TIMEOUT);
}

TEST_CASE("pushed motors are unicast to the plant node with the echoed timestamp")
{
    const std::uint16_t port = pickFreePort();
    DroneSide drone(port);
    FakePlant plant(PLANT_NODE, port, drone.udpLink.dataPort());

    plant.send(makeSensor());
    mark4::SensorFrame frame;
    REQUIRE(drone.source.waitFrame(frame) == mark4::FrameWait::FRAME);

    const mark4::ActuatorFrame actuators = makeActuators(frame.timestampUs);
    drone.sink.push(actuators);
    REQUIRE(drone.sink.pushCount() == 1U);
    REQUIRE(drone.sink.last().motor == actuators.motor);

    const Received reply = plant.receive();
    REQUIRE(reply.size > 0U);
    CHECK(reply.header.src == DRONE_NODE);
    CHECK(reply.header.dst == PLANT_NODE);
    mark4_Envelope envelope;
    REQUIRE(mark4::decodeEnvelope(reply.payload.data(), reply.size, envelope));
    REQUIRE(envelope.which_body == mark4_Envelope_sim_actuator_tag);
    // The reply echoes the sensor timestamp: the lockstep handshake.
    REQUIRE(envelope.body.sim_actuator.echo_timestamp_us == TEST_TIMESTAMP_US);
    for (std::size_t motor = 0U; motor < actuators.motor.size(); ++motor)
    {
        REQUIRE(envelope.body.sim_actuator.motor[motor] == actuators.motor[motor]);
    }
}
