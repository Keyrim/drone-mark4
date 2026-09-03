/// @file
/// @brief GatewayMessage codec and the translations behind it.

#include "hub/gateway_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <pb_decode.h>
#include <pb_encode.h>

#include "log/module.hpp"
#include "log/wire.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_MS = 1000U;
        constexpr std::uint32_t BYTE_MASK = 0xFFU;
        constexpr unsigned BITS_PER_BYTE = 8U;

        // The two enums of the update client are declared in the same order as
        // the wire's; the casts below rely on it.
        static_assert(static_cast<int>(OtaPhase::FAILED) == mark4_OtaState_Phase_FAILED);
        static_assert(static_cast<int>(OtaVerdict::FAILED) ==
                      mark4_OtaState_Verdict_VERDICT_FAILED);
    } // namespace

    void copyWireString(std::string_view text, char *field, std::size_t capacity)
    {
        const std::size_t length = std::min(text.size(), capacity - 1U);
        std::memcpy(field, text.data(), length);
        field[length] = '\0';
    }

    bool encodeGatewayMessage(const mark4_GatewayMessage &message, std::string &out)
    {
        if (message.which_body == 0U)
        {
            return false;
        }
        out.assign(mark4_GatewayMessage_size, '\0');
        pb_ostream_t stream = pb_ostream_from_buffer(reinterpret_cast<std::uint8_t *>(out.data()),
                                                     out.size()); // NOLINT
        if (!pb_encode(&stream, mark4_GatewayMessage_fields, &message))
        {
            out.clear();
            return false;
        }
        out.resize(stream.bytes_written);
        return true;
    }

    bool decodeGatewayMessage(const std::uint8_t *data,
                              std::size_t size,
                              mark4_GatewayMessage &messageOut)
    {
        messageOut = mark4_GatewayMessage_init_zero;
        if (data == nullptr || size == 0U)
        {
            return false;
        }
        pb_istream_t stream = pb_istream_from_buffer(data, size);
        return pb_decode(&stream, mark4_GatewayMessage_fields, &messageOut);
    }

    mark4_OtaState otaStateOf(const OtaClient &client, std::uint32_t targetNode)
    {
        mark4_OtaState state = mark4_OtaState_init_zero;
        state.phase = static_cast<mark4_OtaState_Phase>(client.phase());
        state.verdict = static_cast<mark4_OtaState_Verdict>(client.verdict());
        copyWireString(client.verdictText(), state.verdict_text, sizeof(state.verdict_text));
        copyWireString(client.lastError(), state.last_error, sizeof(state.last_error));
        state.target_node = targetNode;
        state.target_slot = client.targetSlot() == OTA_SLOT_COUNT ? -1 : client.targetSlot();

        const OtaBundle &bundle = client.bundle();
        state.has_bundle = true;
        state.bundle.loaded = bundle.loaded();
        copyWireString(client.bundlePath(), state.bundle.path, sizeof(state.bundle.path));
        copyWireString(bundle.name, state.bundle.name, sizeof(state.bundle.name));
        state.bundle.mcu = bundle.mcuId;
        state.bundle.build_epoch = bundle.buildEpoch;
        copyWireString(bundle.gitHash, state.bundle.git_hash, sizeof(state.bundle.git_hash));
        copyWireString(bundle.wireHash, state.bundle.wire_hash, sizeof(state.bundle.wire_hash));
        state.bundle.image_count = static_cast<std::uint32_t>(bundle.images.size());

        const OtaBoardStatus &board = client.board();
        state.has_board = true;
        state.board.seen = board.seen;
        state.board.mcu = board.mcu;
        state.board.running_slot = board.runningSlot;
        state.board.active_slot = board.activeSlot;
        state.board.updater_busy = board.updaterBusy;
        state.board.slot_size = board.slotSize;
        state.board.max_chunk_data = board.maxChunkData;
        state.board.slots_count = board.seen ? OTA_SLOT_COUNT : 0U;
        for (std::size_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            state.board.slots[slot].state = board.slots[slot].state;
            state.board.slots[slot].build_epoch = board.slots[slot].buildEpoch;
            copyWireString(board.slots[slot].gitHash,
                           state.board.slots[slot].git_hash,
                           sizeof(state.board.slots[slot].git_hash));
        }

        const OtaProgress &progress = client.progress();
        state.has_progress = true;
        state.progress.sent_bytes = progress.sentBytes;
        state.progress.acked_bytes = progress.ackedBytes;
        state.progress.total_bytes = progress.totalBytes;
        state.progress.retries = progress.retries;
        return state;
    }

    bool applyOtaCommand(OtaClient &client,
                         const mark4_OtaCommand &command,
                         std::uint32_t &targetNodeInOut,
                         std::uint64_t nowUs,
                         std::string &errorOut)
    {
        if (command.op == mark4_OtaCommand_Op_ABORT)
        {
            // Dropping a stuck transfer must work whatever the target is.
            return client.abortSession(nowUs, errorOut);
        }
        if (command.target_node == 0U)
        {
            errorOut = "no target node";
            return false;
        }
        if (client.busy() && command.target_node != targetNodeInOut)
        {
            errorOut = "a session runs against node " + std::to_string(targetNodeInOut);
            return false;
        }
        targetNodeInOut = command.target_node;
        switch (command.op)
        {
            case mark4_OtaCommand_Op_START:
                return client.start(command.bundle_path, nowUs, errorOut);
            case mark4_OtaCommand_Op_REVERT:
                return client.revert(nowUs, errorOut);
            case mark4_OtaCommand_Op_STATUS_REQUEST:
                return client.requestBoardStatus(nowUs, errorOut);
            default:
                errorOut = "unsupported update command";
                return false;
        }
    }

    bool pushProfile(const TuningProfiles &profiles,
                     std::string_view name,
                     std::uint32_t dst,
                     const EnvelopeSink &sink,
                     std::string &errorOut)
    {
        if (dst == 0U)
        {
            errorOut = "no target node";
            return false;
        }
        TuningValues values;
        if (!profiles.load(name, values, errorOut))
        {
            return false;
        }
        for (const auto &[id, value] : values)
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_tuning_set_tag;
            envelope.body.tuning_set.id = id;
            envelope.body.tuning_set.value = value;
            if (!sink(dst, envelope, errorOut))
            {
                return false;
            }
        }
        return true;
    }

    TuningValues tuningValuesOf(const mark4_TuningSet *values, std::size_t count)
    {
        TuningValues out;
        for (std::size_t i = 0U; i < count; ++i)
        {
            out[static_cast<std::uint16_t>(values[i].id)] = values[i].value; // NOLINT
        }
        return out;
    }

    void fillProfile(std::string_view name, const TuningValues &values, mark4_Profile &profileOut)
    {
        profileOut = mark4_Profile_init_zero;
        copyWireString(name, profileOut.name, sizeof(profileOut.name));
        for (const auto &[id, value] : values)
        {
            if (profileOut.values_count >= std::size(profileOut.values))
            {
                break;
            }
            profileOut.values[profileOut.values_count].id = id;
            profileOut.values[profileOut.values_count].value = value;
            ++profileOut.values_count;
        }
    }

    void fillNode(const Transport::Node &node,
                  std::uint64_t nowUs,
                  const mark4_Announce *announce,
                  const LogModuleTable &logModules,
                  mark4_Node &nodeOut)
    {
        nodeOut = mark4_Node_init_zero;
        nodeOut.id = node.id;
        if (node.address.host != 0U)
        {
            static_cast<void>(std::snprintf(nodeOut.address,
                                            sizeof(nodeOut.address),
                                            "%u.%u.%u.%u",
                                            (node.address.host >> (3U * BITS_PER_BYTE)) & BYTE_MASK,
                                            (node.address.host >> (2U * BITS_PER_BYTE)) & BYTE_MASK,
                                            (node.address.host >> BITS_PER_BYTE) & BYTE_MASK,
                                            node.address.host & BYTE_MASK));
        }
        nodeOut.port = node.address.port;
        nodeOut.last_seen_ms_ago =
            static_cast<std::uint32_t>((nowUs - std::min(nowUs, node.lastSeenUs)) / US_PER_MS);
        nodeOut.received = node.received;
        nodeOut.lost = node.lost;
        nodeOut.duplicates = node.duplicates;
        if (announce != nullptr)
        {
            nodeOut.has_announce = true;
            nodeOut.announce = *announce;
        }
        const std::size_t count = std::min(logModules.size(), std::size(nodeOut.log_modules));
        std::copy_n(logModules.begin(), count, nodeOut.log_modules);
        nodeOut.log_modules_count = static_cast<pb_size_t>(count);
    }

    void applyLogModulesPage(const mark4_LogModules &page, LogModuleTable &tableInOut)
    {
        if (page.start_index == 0U)
        {
            tableInOut.clear();
        }
        tableInOut.resize(page.total);
        for (pb_size_t i = 0U; i < page.modules_count; ++i)
        {
            const std::size_t index = page.start_index + i;
            if (index < tableInOut.size())
            {
                tableInOut[index] = page.modules[i];
            }
        }
    }

    std::uint32_t applyTelemetryPage(const mark4_TelemetryDescriptors &page,
                                     TelemetryTable &tableInOut)
    {
        if (page.cursor == 0U)
        {
            tableInOut.clear();
        }
        tableInOut.resize(page.total);
        for (pb_size_t i = 0U; i < page.descriptors_count; ++i)
        {
            const std::size_t index = page.cursor + i;
            if (index < tableInOut.size())
            {
                tableInOut[index] = page.descriptors[i];
            }
        }
        // A page that carried nothing while the table is not full would loop
        // forever on the same cursor: the total is what closes the walk.
        const std::uint32_t next = page.cursor + page.descriptors_count;
        return page.descriptors_count == 0U ? page.total : next;
    }

    void fillNodeTelemetry(std::uint32_t node,
                           const TelemetryTable &table,
                           mark4_NodeTelemetry &out)
    {
        out = mark4_NodeTelemetry_init_zero;
        out.node = node;
        const std::size_t count = std::min(table.size(), std::size(out.descriptors));
        std::copy_n(table.begin(), count, out.descriptors);
        out.descriptors_count = static_cast<pb_size_t>(count);
    }

    LogModuleTable ownLogModules()
    {
        LogModuleTable table;
        for (const LogModule *module = logModules(); module != nullptr; module = module->next())
        {
            mark4_LogModuleInfo info = mark4_LogModuleInfo_init_zero;
            info.id = module->id();
            copyWireString(module->name(), info.name, sizeof(info.name));
            info.level = logLevelToWire(module->level());
            table.push_back(info);
        }
        return table;
    }

    std::string hexNodeId(std::uint32_t id)
    {
        static constexpr std::size_t HEX_DIGITS = 8U;
        std::array<char, HEX_DIGITS + 1U> text{};
        static_cast<void>(std::snprintf(text.data(), text.size(), "%08x", id));
        return {text.data()};
    }
} // namespace mark4
