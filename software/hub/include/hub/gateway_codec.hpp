#pragma once

/// @file
/// @brief The gateway side of gateway.proto: the codec of GatewayMessage and
///        the few translations between the hub's own services (update
///        client, profiles, transport table) and the messages a client
///        reads. Pure functions, no socket, no state.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "gateway.pb.h"
#include "hub/ota_client.hpp"
#include "hub/tuning_profiles.hpp"
#include "protocol/envelope.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    /// Route one envelope to one node: what a profile push sends through.
    using EnvelopeSink =
        std::function<bool(std::uint32_t dst, const mark4_Envelope &, std::string &errorOut)>;

    /// @brief Encodes one GatewayMessage into a websocket binary message.
    /// @param message message to encode; which_body must name a body
    /// @param[out] out receives the bytes, replaced
    /// @return false when the message does not encode
    bool encodeGatewayMessage(const mark4_GatewayMessage &message, std::string &out);

    /// @brief Decodes one websocket binary message.
    /// @param data bytes
    /// @param size byte count
    /// @param[out] messageOut decoded message, zeroed first
    /// @return false when the bytes are not a GatewayMessage
    bool decodeGatewayMessage(const std::uint8_t *data,
                              std::size_t size,
                              mark4_GatewayMessage &messageOut);

    /// @brief Snapshots the update client as the message a client paints.
    /// @param client update client
    /// @param targetNode node the session talks to, 0 when none was chosen
    /// @return the state
    mark4_OtaState otaStateOf(const OtaClient &client, std::uint32_t targetNode);

    /// @brief Carries out one update command. The target is fixed for the
    ///        whole session: a command naming another node while a session
    ///        runs is refused, so half an image never lands elsewhere.
    /// @param client update client to drive
    /// @param command what the client asked
    /// @param[in,out] targetNodeInOut node the update client talks to
    /// @param nowUs current time [us]
    /// @param[out] errorOut receives the refusal reason
    /// @return true when the command was carried out
    bool applyOtaCommand(OtaClient &client,
                         const mark4_OtaCommand &command,
                         std::uint32_t &targetNodeInOut,
                         std::uint64_t nowUs,
                         std::string &errorOut);

    /// @brief Sends one whole profile to a node, one TuningSet per value.
    /// @param profiles the profiles on disk
    /// @param name profile to push
    /// @param dst node to push it to
    /// @param sink route to that node
    /// @param[out] errorOut receives the reason on failure
    /// @return true when every value went out
    bool pushProfile(const TuningProfiles &profiles,
                     std::string_view name,
                     std::uint32_t dst,
                     const EnvelopeSink &sink,
                     std::string &errorOut);

    /// @brief Reads the value pairs of a ProfileCommand.
    /// @param values pairs
    /// @param count pair count
    /// @return the values, by parameter id
    TuningValues tuningValuesOf(const mark4_TuningSet *values, std::size_t count);

    /// @brief Fills one Profile message.
    /// @param name profile name
    /// @param values its values
    /// @param[out] profileOut receives them, truncated to the wire bound
    void fillProfile(std::string_view name, const TuningValues &values, mark4_Profile &profileOut);

    /// @brief Fills one Node entry of the table from the transport's record.
    /// @param node transport record
    /// @param nowUs current time [us], turned into an age
    /// @param announce last beacon of that node, nullptr when none
    /// @param[out] nodeOut receives the entry
    void fillNode(const Transport::Node &node,
                  std::uint64_t nowUs,
                  const mark4_Announce *announce,
                  mark4_Node &nodeOut);

    /// @brief Copies a string into a fixed wire field, truncating.
    /// @param text source
    /// @param[out] field destination
    /// @param capacity bytes of field, terminator included
    void copyWireString(std::string_view text, char *field, std::size_t capacity);
} // namespace mark4
