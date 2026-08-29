/// @file
/// @brief The one cross-language check of the wire and of the transport: a
///        headless Godot runs the GDScript transport and the generated
///        codec (sim-godot/tests/plant_link_check.gd) against this node,
///        exactly the way the plant and the flight process do on the sim
///        link. Skips with a message when no godot binary was found at
///        configure time.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "platform_sim/clock_sim.hpp"
#include "protocol/envelope.hpp"
#include "protocol/wire_hash.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

namespace
{
    /// How long the plant may take to boot and speak [ms]. Generous: a cold
    /// Godot under the sanitizers takes a few seconds to come up.
    constexpr std::uint64_t PLANT_BUDGET_MS = 30000U;
    constexpr std::uint64_t US_PER_MS = 1000U;
    constexpr std::uint32_t DRONE_NODE = 0xD0000002U;

    /// @return a free UDP port of this host, released again on return
    std::uint16_t pickFreePort()
    {
        mark4::UdpLink probe(0U);
        REQUIRE(probe.init());
        return probe.dataPort();
    }

    /// What the plant sent, captured by the transport delivery callback.
    struct Delivered
    {
        std::uint32_t src = 0U;                             ///< the plant's node id
        mark4_Envelope envelope = mark4_Envelope_init_zero; ///< the SimSensor envelope
        bool sensorSeen = false;                            ///< a SimSensor arrived
    };

    void onPayload(void *context, std::uint32_t src, const std::uint8_t *payload, std::size_t size)
    {
        auto &delivered = *static_cast<Delivered *>(context);
        mark4_Envelope envelope;
        if (mark4::decodeEnvelope(payload, size, envelope) &&
            envelope.which_body == mark4_Envelope_sim_sensor_tag)
        {
            delivered.src = src;
            delivered.envelope = envelope;
            delivered.sensorSeen = true;
        }
    }

    /// @brief Encodes and unicasts one envelope to a node.
    void sendEnvelope(mark4::Transport &transport,
                      std::uint32_t dst,
                      const mark4_Envelope &envelope)
    {
        std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
        REQUIRE(transport.send(dst, bytes.data(), size));
    }
} // namespace

TEST_CASE("the plant's GDScript transport and codec agree with the C++ ones", "[godot]")
{
    const std::string godot = MARK4_GODOT_BINARY;
    const std::filesystem::path project = MARK4_GODOT_PROJECT;
    if (godot.empty() || godot.find("NOTFOUND") != std::string::npos)
    {
        SKIP("no godot binary on the PATH at configure time");
    }
    if (!std::filesystem::exists(project / "scripts" / "gen" / "mark4.gd"))
    {
        SKIP("sim-godot/scripts/gen/mark4.gd was not generated (proto_gd target)");
    }

    // The flight process side: a transport node announcing itself as a
    // DRONE_SIM on a private discovery port, so the plant finds it by its
    // beacon like it finds drone_sim.
    const std::uint16_t discoveryPort = pickFreePort();
    mark4::UdpLink link(discoveryPort);
    REQUIRE(link.init());
    mark4::Transport transport(DRONE_NODE);
    REQUIRE(transport.addLink(link));
    REQUIRE(transport.init());
    mark4_Envelope announce = mark4_Envelope_init_zero;
    announce.which_body = mark4_Envelope_announce_tag;
    announce.body.announce.kind = mark4_NodeKind_DRONE_SIM;
    announce.body.announce.mcu = mark4_Mcu_SIM;
    announce.body.announce.wire_hash = mark4::WIRE_HASH;
    std::array<std::uint8_t, mark4::Transport::MAX_BEACON_SIZE> beacon{};
    std::size_t beaconSize = 0U;
    REQUIRE(mark4::encodeEnvelope(announce, beacon.data(), beacon.size(), beaconSize));
    transport.setBeacon(beacon.data(), beaconSize);
    mark4::ClockSim clock;

    const std::string port = std::to_string(discoveryPort);
    const std::string projectText = project.string();
    std::array<const char *, 10> argv = {godot.c_str(),
                                         "--headless",
                                         "--path",
                                         projectText.c_str(),
                                         "--script",
                                         "tests/plant_link_check.gd",
                                         "--",
                                         "--discovery-port",
                                         port.c_str(),
                                         nullptr};
    pid_t pid = -1;
    REQUIRE(::posix_spawn(&pid,
                          godot.c_str(),
                          nullptr,
                          nullptr,
                          const_cast<char *const *>(argv.data()),
                          nullptr) == 0);

    // The plant speaks first, like on the sim link.
    Delivered delivered;
    const std::uint64_t deadlineUs = clock.nowUs() + PLANT_BUDGET_MS * US_PER_MS;
    while (!delivered.sensorSeen && clock.nowUs() < deadlineUs)
    {
        transport.poll(clock.nowUs(), &onPayload, &delivered);
        ::usleep(2000);
    }
    REQUIRE(delivered.sensorSeen);
    REQUIRE(transport.isAlive(delivered.src));
    const mark4_SimSensor &sensor = delivered.envelope.body.sim_sensor;
    CHECK(sensor.timestamp_us == 42U);
    CHECK(sensor.gyro_rad_s[0] == 0.25f);
    CHECK(sensor.gyro_rad_s[1] == -0.5f);
    CHECK(sensor.gyro_rad_s[2] == 1.5f);
    CHECK(sensor.accel_mps2[2] == 9.80665f);
    CHECK(sensor.baro_pa == 101325.0f);
    CHECK(sensor.reset_count == 3U);
    CHECK(sensor.lockstep_timeouts == 7U);
    REQUIRE(sensor.has_truth);
    CHECK(sensor.truth.attitude_quat[0] == 1.0f);
    CHECK(sensor.truth.position_m[2] == 1.5f);
    CHECK(sensor.truth.velocity_mps[0] == -2.0f);

    // Both replies the flight process unicasts to its plant.
    mark4_Envelope actuator = mark4_Envelope_init_zero;
    actuator.which_body = mark4_Envelope_sim_actuator_tag;
    actuator.body.sim_actuator.echo_timestamp_us = sensor.timestamp_us;
    actuator.body.sim_actuator.motor[0] = 0.1f;
    actuator.body.sim_actuator.motor[1] = 0.2f;
    actuator.body.sim_actuator.motor[2] = 0.3f;
    actuator.body.sim_actuator.motor[3] = 0.4f;
    sendEnvelope(transport, delivered.src, actuator);

    mark4_Envelope scenario = mark4_Envelope_init_zero;
    scenario.which_body = mark4_Envelope_sim_scenario_tag;
    scenario.body.sim_scenario.sequence = 3U;
    scenario.body.sim_scenario.kind = mark4_SimScenarioKind_THROW;
    scenario.body.sim_scenario.velocity_mps[2] = 6.5f;
    sendEnvelope(transport, delivered.src, scenario);

    // The plant judges the replies and says so with its exit code.
    int status = 0;
    bool exited = false;
    for (std::uint64_t waited = 0U; waited < PLANT_BUDGET_MS && !exited; waited += 10U)
    {
        exited = ::waitpid(pid, &status, WNOHANG) == pid;
        if (!exited)
        {
            ::usleep(10000);
        }
    }
    if (!exited)
    {
        static_cast<void>(::kill(pid, SIGKILL));
        static_cast<void>(::waitpid(pid, &status, 0));
    }
    REQUIRE(exited);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
