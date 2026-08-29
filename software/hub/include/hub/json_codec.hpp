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
#include "hub/ota_client.hpp"
#include "hub/stream_health.hpp"
#include "hub/tuning_profiles.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"
#include "protocol/sim_raw.hpp"
#include "protocol/telemetry.hpp"
#include "protocol/tuning.hpp"

namespace mark4
{
    /// What a websocket client asked for.
    enum class ClientMessageType : std::uint8_t
    {
        RC,           ///< pilot state to forward to a flight process
        SIM_SCENARIO, ///< run to play, forwarded to a flight process
        REBOOT,       ///< reset the real board
        TUNING_SET,   ///< write one tunable parameter
        TUNING_GET,   ///< read one tunable parameter
        TUNING_LIST,  ///< walk the parameter table
        PROFILE_LIST, ///< name the stored tuning profiles
        PROFILE_SAVE, ///< store one named set of values
        PROFILE_LOAD, ///< read one named set of values back
        PROFILE_PUSH, ///< send one named set to a flight process
        CONNECT,      ///< make one drone THE connected drone
        DISCONNECT,   ///< drop the connected drone
        OTA_STATUS,   ///< ask the board what firmware it runs
        OTA_START,    ///< send one .ota bundle to the board
        OTA_ABORT,    ///< drop the running update
        OTA_REVERT,   ///< activate the other firmware slot
    };

    /// One decoded client request. The wire packets are already built: the
    /// codec owns every version and magic byte, the caller only routes bytes.
    struct ClientMessage
    {
        ClientMessageType type = ClientMessageType::RC; ///< which request this is
        int id = -1;                                    ///< client correlation id,
                                                        ///< -1 when the client sent none
        StreamSource target = StreamSource::FIRMWARE;   ///< command destination
        RcCommandPacket rc{};                           ///< RC: packet to forward as is
        SimScenarioPacket simScenario{};                ///< SIM_SCENARIO: packet to send
        RebootCommandPacket reboot{};                   ///< REBOOT: packet to send, magic included
        TuningSetPacket tuningSet{};                    ///< TUNING_SET: packet to forward
        TuningGetPacket tuningGet{};                    ///< TUNING_GET: packet to forward
        TuningListPacket tuningList{};                  ///< TUNING_LIST: packet to forward
        std::string profileName;                        ///< PROFILE_*: profile concerned
        TuningValues profileValues;                     ///< PROFILE_SAVE: values to store
        std::string connectVia;                         ///< CONNECT: "udp" or "bridge"
        std::string connectPeer;                        ///< CONNECT: bridge name
        std::string otaBundlePath;                      ///< OTA_START: bundle to send, empty
                                                        ///< for the standard build output
    };

    /// Counters and flags the hub publishes once per second.
    struct HubStatus
    {
        bool serialOpen = false;   ///< the serial link is usable
        std::string serialLink;    ///< device the link is open on, empty = none
        std::string connectionVia; ///< "udp" or "bridge", empty = none
        std::string connectionId;  ///< kind name or bridge name
        StreamSource connectionKind = StreamSource::FIRMWARE; ///< kind commands route to
        bool connectionLive = false;          ///< the connected drone shows signs of life
        std::uint64_t badFrames = 0U;         ///< serial frames that decoded to nothing
        std::uint64_t rejectedAnnounces = 0U; ///< announces dropped as invalid
        std::size_t clients = 0U;             ///< websocket clients connected
        std::size_t rcClients = 0U;           ///< clients that streamed RC recently
        std::vector<LinkHealth> links;        ///< sequence health, one entry per link
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

    /// @brief Renders one tuning acknowledgement as a JSON object.
    /// @param packet packet to render
    /// @param source process the answer came from
    /// @return one line of JSON
    std::string tuningAckToJson(const TuningAckPacket &packet, StreamSource source);

    /// @brief Renders one parameter description as a JSON object. The wire
    ///        name is zero-padded and a full-length one carries no
    ///        terminator, so it is read by bounded length, never by strlen.
    /// @param packet packet to render
    /// @param source process the description came from
    /// @return one line of JSON
    std::string tuningInfoToJson(const TuningInfoPacket &packet, StreamSource source);

    /// @brief Renders the names of the stored profiles as a JSON object.
    /// @param names profile names
    /// @return one line of JSON
    std::string profileNamesToJson(const std::vector<std::string> &names);

    /// @brief Renders one profile as a JSON object.
    /// @param name profile name
    /// @param values values it holds
    /// @return one line of JSON
    std::string profileToJson(const std::string &name, const TuningValues &values);

    /// @brief Renders the whole discovery table as a JSON object: the live
    ///        processes, and the WiFi bridges a serial link can be opened on.
    /// @param processes live entries
    /// @param bridges bridges heard announcing themselves
    /// @param nowUs current time [us], turned into an age per entry
    /// @return one line of JSON
    std::string discoveryToJson(const std::vector<DiscoveredProcess> &processes,
                                const std::vector<DiscoveredBridge> &bridges,
                                std::uint64_t nowUs);

    /// @brief Renders the whole state of the update client as a JSON object:
    ///        what the board runs, what the bundle holds, where the session
    ///        stands, how far the transfer got and what it concluded. One
    ///        message carries all of it, so a page that just connected and a
    ///        page that has been watching read the same shape.
    /// @param client update client to render
    /// @return one line of JSON
    std::string otaToJson(const OtaClient &client);

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
