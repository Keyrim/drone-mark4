/// @file
/// @brief The one cross-language check of the wire: a headless Godot runs
///        the generated GDScript codec (sim-godot/tests/plant_link_check.gd)
///        and exchanges envelopes with the nanopb codec over UDP, exactly the
///        way the plant and the flight process do on the lockstep link. Skips
///        with a message when no godot binary was found at configure time.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "protocol/envelope.hpp"

namespace
{
    /// How long the plant may take to boot and speak [ms]. Generous: a cold
    /// Godot under the sanitizers takes a few seconds to come up.
    constexpr int PLANT_BUDGET_MS = 30000;

    /// @brief Encodes and sends one envelope to a peer.
    /// @param fd socket to send on
    /// @param peer where to send
    /// @param envelope message
    void sendEnvelope(int fd, const sockaddr_in &peer, const mark4_Envelope &envelope)
    {
        std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
        REQUIRE(::sendto(fd,
                         bytes.data(),
                         size,
                         0,
                         reinterpret_cast<const sockaddr *>(&peer),
                         sizeof(peer)) == static_cast<ssize_t>(size));
    }
} // namespace

TEST_CASE("the plant's generated codec and the nanopb codec agree over UDP", "[godot]")
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

    // The flight process side: one socket on an ephemeral loopback port.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) == 0);
    socklen_t length = sizeof(local);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &length) == 0);
    const std::string port = std::to_string(ntohs(local.sin_port));

    const std::string projectText = project.string();
    std::array<const char *, 9> argv = {godot.c_str(),
                                        "--headless",
                                        "--path",
                                        projectText.c_str(),
                                        "--script",
                                        "tests/plant_link_check.gd",
                                        "--",
                                        "--port",
                                        port.c_str()};
    std::array<const char *, 10> argvWithEnd{};
    std::copy(argv.begin(), argv.end(), argvWithEnd.begin());
    pid_t pid = -1;
    REQUIRE(::posix_spawn(&pid,
                          godot.c_str(),
                          nullptr,
                          nullptr,
                          const_cast<char *const *>(argvWithEnd.data()),
                          nullptr) == 0);

    // The plant speaks first, like on the lockstep link.
    pollfd entry{fd, POLLIN, 0};
    REQUIRE(::poll(&entry, 1U, PLANT_BUDGET_MS) == 1);
    std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE + 64U> bytes{};
    sockaddr_in peer{};
    socklen_t peerLength = sizeof(peer);
    const ssize_t received = ::recvfrom(
        fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peerLength);
    REQUIRE(received > 0);

    mark4_Envelope envelope;
    REQUIRE(mark4::decodeEnvelope(bytes.data(), static_cast<std::size_t>(received), envelope));
    REQUIRE(envelope.which_body == mark4_Envelope_sim_sensor_tag);
    const mark4_SimSensor &sensor = envelope.body.sim_sensor;
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

    // Both replies the flight process sends on that link.
    mark4_Envelope actuator = mark4_Envelope_init_zero;
    actuator.which_body = mark4_Envelope_sim_actuator_tag;
    actuator.body.sim_actuator.echo_timestamp_us = sensor.timestamp_us;
    actuator.body.sim_actuator.motor[0] = 0.1f;
    actuator.body.sim_actuator.motor[1] = 0.2f;
    actuator.body.sim_actuator.motor[2] = 0.3f;
    actuator.body.sim_actuator.motor[3] = 0.4f;
    sendEnvelope(fd, peer, actuator);

    mark4_Envelope scenario = mark4_Envelope_init_zero;
    scenario.which_body = mark4_Envelope_sim_scenario_tag;
    scenario.body.sim_scenario.sequence = 3U;
    scenario.body.sim_scenario.kind = mark4_SimScenarioKind_THROW;
    scenario.body.sim_scenario.velocity_mps[2] = 6.5f;
    sendEnvelope(fd, peer, scenario);

    // The plant judges the replies and says so with its exit code.
    int status = 0;
    bool exited = false;
    for (int waited = 0; waited < PLANT_BUDGET_MS && !exited; waited += 10)
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
    static_cast<void>(::close(fd));
    REQUIRE(exited);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
