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
        SIM_COMMAND,  ///< scenario command for the simulator
        REBOOT,       ///< reset the real board
        RECORD,       ///< start or stop the CSV recording
        TUNING_SET,   ///< write one tunable parameter
        TUNING_GET,   ///< read one tunable parameter
        TUNING_LIST,  ///< walk the parameter table
        PROFILE_LIST, ///< name the stored tuning profiles
        PROFILE_SAVE, ///< store one named set of values
        PROFILE_LOAD, ///< read one named set of values back
        PROFILE_PUSH  ///< send one named set to a flight process
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
        TuningSetPacket tuningSet{};                    ///< TUNING_SET: packet to forward
        TuningGetPacket tuningGet{};                    ///< TUNING_GET: packet to forward
        TuningListPacket tuningList{};                  ///< TUNING_LIST: packet to forward
        std::string profileName;                        ///< PROFILE_*: profile concerned
        TuningValues profileValues;                     ///< PROFILE_SAVE: values to store
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
