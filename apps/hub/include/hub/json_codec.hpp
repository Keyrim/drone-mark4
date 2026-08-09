#pragma once

/// @file
/// @brief Translation between the binary wire of protocol/ and the JSON a
///        browser or a script can read. Pure functions, no socket, no state:
///        one side takes a packet and returns text, the other takes text and
///        returns a ready-made wire packet. This is the only place in the
///        repo where a protocol/ field name becomes a JSON key.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "hub/discovery.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"
#include "protocol/sim_raw.hpp"
#include "protocol/telemetry.hpp"

namespace mark4
{
    /// What a websocket client asked for.
    enum class ClientMessageType : std::uint8_t
    {
        RC,          ///< pilot state to forward to a flight process
        SIM_COMMAND, ///< scenario command for the simulator
        REBOOT,      ///< reset the real board
        RECORD       ///< start or stop the CSV recording
    };

    /// One decoded client request. The wire packets are already built: the
    /// codec owns every version and magic byte, the caller only routes bytes.
    struct ClientMessage
    {
        ClientMessageType type = ClientMessageType::RC; ///< which request this is
        int id = -1;                                    ///< client correlation id,
                                                        ///< -1 when the client sent none
        StreamSource target = StreamSource::FIRMWARE;   ///< RC and REBOOT destination
        RcCommandPacket rc{};                           ///< RC: packet to forward as is
        SimCommandPacket simCommand{};                  ///< SIM_COMMAND: packet to send
        RebootCommandPacket reboot{};                   ///< REBOOT: packet to send, magic included
        bool recordStart = false;                       ///< RECORD: true = start, false = stop
    };

    /// Counters and flags the hub publishes once per second.
    struct HubStatus
    {
        bool recording = false;               ///< a CSV session is open
        bool serialOpen = false;              ///< the serial link is usable
        std::uint64_t telemetryRows = 0U;     ///< telemetry rows written
        std::uint64_t simRawRows = 0U;        ///< sim raw rows written
        std::uint64_t blackboxRecords = 0U;   ///< blackbox records written
        std::uint64_t badFrames = 0U;         ///< serial frames that decoded to nothing
        std::uint64_t rejectedAnnounces = 0U; ///< announces dropped as invalid
        std::size_t clients = 0U;             ///< websocket clients connected
    };

    /// @brief Serialized bytes of a packed wire struct, ready for a socket.
    ///        The wire structs are trivially copyable and padding-free, so the
    ///        copy is the exact byte sequence the protocol describes.
    /// @param packet packet to serialize
    /// @return the packet bytes
    template <typename Packet>
    std::array<std::uint8_t, sizeof(Packet)> wireBytes(const Packet &packet)
    {
        static_assert(std::is_trivially_copyable_v<Packet>);
        std::array<std::uint8_t, sizeof(Packet)> bytes{};
        std::memcpy(bytes.data(), &packet, sizeof(Packet));
        return bytes;
    }

    /// @brief Renders one telemetry packet as a JSON object.
    /// @param packet packet to render
    /// @return one line of JSON, keys named exactly like the struct fields
    std::string telemetryToJson(const TelemetryPacket &packet);

    /// @brief Renders one raw simulator state packet as a JSON object.
    /// @param packet packet to render
    /// @return one line of JSON, keys named exactly like the struct fields
    std::string simRawToJson(const SimRawPacket &packet);

    /// @brief Renders the whole discovery table as a JSON object.
    /// @param processes live entries
    /// @param nowUs current time [us], turned into an age per entry
    /// @return one line of JSON
    std::string discoveryToJson(const std::vector<DiscoveredProcess> &processes,
                                std::uint64_t nowUs);

    /// @brief Renders the hub counters as a JSON object.
    /// @param status counters and flags to render
    /// @return one line of JSON
    std::string statusToJson(const HubStatus &status);

    /// @brief Renders the answer to a client request that carried an id.
    /// @param id correlation id the client sent
    /// @param ok true when the request was carried out
    /// @param error reason of the refusal, empty when ok
    /// @return one line of JSON
    std::string ackToJson(int id, bool ok, std::string_view error);

    /// @brief Decodes one text message received from a client. Never throws:
    ///        a malformed message comes back as the error alternative.
    /// @param text raw message text
    /// @return the decoded request, or the reason it could not be decoded
    std::variant<ClientMessage, std::string> parseClientMessage(std::string_view text);

    /// @brief Best-effort extraction of the correlation id of a message that
    ///        failed to decode, so even a rejected request can be answered.
    /// @param text raw message text
    /// @return the id, -1 when the message carries none
    int clientMessageId(std::string_view text);
} // namespace mark4
