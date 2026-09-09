/// @file
/// @brief The updater on the wire: a request from one node is answered to
///        that node, and what is not an updater request never reaches it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "messaging/messenger.hpp"
#include "ota/updater.hpp"
#include "platform_sim/firmware_store_sim.hpp"
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"
#include "recording_link.hpp"
#include "services/ota_service.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace
{
    constexpr std::uint32_t NODE_SELF = 0x07A00001U;
    constexpr std::uint32_t NODE_GROUND = 0x67000001U;
    constexpr std::uint64_t T0_US = 1'000'000U;
    constexpr std::uint32_t TEST_SLOT_SIZE = 8192U;

    /// @param name directory name under the temp directory
    /// @return an absolute path with nothing in it
    std::string scratchDirectory(const char *name)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::error_code failure;
        std::filesystem::remove_all(path, failure);
        return path.string();
    }

    /// The whole node: a messenger over a transport over a recording link,
    /// the service over an updater over the file-backed store, composed in
    /// the order a flight process composes them.
    class Node
    {
      public:
        explicit Node(const std::string &directory)
            : m_store(directory.c_str(), mark4::OTA_SLOT_A, TEST_SLOT_SIZE)
        {
            REQUIRE(m_store.init());
            static_cast<void>(m_transport.addLink(m_link));
            // The requester is learnt up front, so a unicast to it can leave.
            m_link.deliver(NODE_GROUND, NODE_SELF, {0x00U});
            m_messenger.poll(T0_US);
            REQUIRE(m_transport.isAlive(NODE_GROUND));
            m_link.clear();
        }

        /// @brief Delivers one message from the ground node and polls once.
        /// @param envelope message to deliver
        void request(const mark4_Envelope &envelope)
        {
            std::vector<std::uint8_t> bytes(mark4::MAX_ENVELOPE_SIZE, 0U);
            std::size_t size = 0U;
            REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
            bytes.resize(size);
            m_link.deliver(NODE_GROUND, NODE_SELF, bytes);
            m_messenger.poll(T0_US);
        }

        [[nodiscard]] const mark4::RecordingLink &link() const
        {
            return m_link;
        }

        [[nodiscard]] const mark4::Messenger &messenger() const
        {
            return m_messenger;
        }

        [[nodiscard]] const mark4::OtaService &service() const
        {
            return m_service;
        }

      private:
        mark4::RecordingLink m_link;
        mark4::Transport m_transport{NODE_SELF};
        mark4::Messenger m_messenger{m_transport};
        mark4::FlightCore m_core;
        mark4::FirmwareStoreSim m_store;
        mark4::OtaUpdater m_updater{m_store};
        mark4::OtaService m_service{m_messenger, m_updater, m_core};
    };

    /// @param tag body tag of an empty-bodied message
    /// @return the envelope
    mark4_Envelope bareEnvelope(pb_size_t tag)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = tag;
        return envelope;
    }
} // namespace

TEST_CASE("an updater request is answered to the node that asked")
{
    const std::string directory = scratchDirectory("mark4_ota_service_status");
    Node node(directory);

    node.request(bareEnvelope(mark4_Envelope_ota_status_request_tag));
    REQUIRE(node.service().consumed() == 1U);
    REQUIRE(node.messenger().handled() == 1U);

    REQUIRE(node.link().frames().size() == 1U);
    const mark4::RecordedFrame &frame = node.link().frames()[0];
    REQUIRE(!frame.broadcast);
    REQUIRE(frame.header.src == NODE_SELF);
    REQUIRE(frame.header.dst == NODE_GROUND);
    const std::optional<mark4_Envelope> reply = node.link().envelope(0U);
    REQUIRE(reply.has_value());
    REQUIRE(reply->which_body == mark4_Envelope_ota_status_tag);
    REQUIRE(reply->body.ota_status.running_slot == mark4::OTA_SLOT_A);

    std::error_code failure;
    std::filesystem::remove_all(directory, failure);
}

TEST_CASE("a message that is not an updater request never reaches the updater")
{
    const std::string directory = scratchDirectory("mark4_ota_service_reboot");
    Node node(directory);

    // A Reboot is the composition's business: nothing claimed it here, the
    // messenger counts it, the updater consumed nothing and answered nothing.
    node.request(bareEnvelope(mark4_Envelope_reboot_tag));
    REQUIRE(node.messenger().unhandled() == 1U);
    REQUIRE(node.service().consumed() == 0U);
    REQUIRE(node.link().frames().empty());

    std::error_code failure;
    std::filesystem::remove_all(directory, failure);
}
