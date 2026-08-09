/// @file
/// @brief JSON codec implementation. Every conversion from a float field to
///        JSON goes through an explicit widening cast: the wire is
///        single-precision, JSON numbers are double, and the promotion must
///        be written down rather than happen behind our back.

#include "hub/json_codec.hpp"

#include <nlohmann/json.hpp>

#include "hub/packed_field.hpp"

namespace mark4
{
    namespace
    {
        /// Insertion-ordered JSON, so a message reads in the order the wire
        /// struct declares its fields instead of in alphabetical order.
        using Json = nlohmann::ordered_json;

        /// @brief Renders a fixed-size float array as a JSON array.
        /// @param values values to render
        /// @return JSON array of numbers
        template <std::size_t N> Json floatsToJson(const std::array<float, N> &values)
        {
            Json array = Json::array();
            for (const float value : values)
            {
                array.push_back(static_cast<double>(value));
            }
            return array;
        }

        /// @brief Reads an optional number field into a float.
        /// @param object object to read from
        /// @param key field name
        /// @param valueOut receives the value, untouched when the field is absent
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent or a number
        bool readFloat(const Json &object, const char *key, float &valueOut, std::string &errorOut)
        {
            const auto found = object.find(key);
            if (found == object.end())
            {
                return true;
            }
            if (!found->is_number())
            {
                errorOut = std::string("field '") + key + "' must be a number";
                return false;
            }
            valueOut = static_cast<float>(found->get<double>());
            return true;
        }

        /// @brief Reads an optional unsigned integer field into a byte.
        /// @param object object to read from
        /// @param key field name
        /// @param valueOut receives the value, untouched when the field is absent
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent or an integer in [0, 255]
        bool readByte(const Json &object,
                      const char *key,
                      std::uint8_t &valueOut,
                      std::string &errorOut)
        {
            static constexpr int MAX_BYTE = 255;
            const auto found = object.find(key);
            if (found == object.end())
            {
                return true;
            }
            if (!found->is_number_integer())
            {
                errorOut = std::string("field '") + key + "' must be an integer";
                return false;
            }
            const auto raw = found->get<std::int64_t>();
            if (raw < 0 || raw > MAX_BYTE)
            {
                errorOut = std::string("field '") + key + "' must be in [0, 255]";
                return false;
            }
            valueOut = static_cast<std::uint8_t>(raw);
            return true;
        }

        /// @brief Reads an optional three-number array field.
        /// @param object object to read from
        /// @param key field name
        /// @param valueOut receives the values, untouched when the field is absent
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent or an array of three numbers
        bool readVector(const Json &object,
                        const char *key,
                        std::array<float, 3> &valueOut,
                        std::string &errorOut)
        {
            const auto found = object.find(key);
            if (found == object.end())
            {
                return true;
            }
            if (!found->is_array() || found->size() != valueOut.size())
            {
                errorOut = std::string("field '") + key + "' must be an array of 3 numbers";
                return false;
            }
            for (std::size_t axis = 0U; axis < valueOut.size(); ++axis)
            {
                const Json &component = (*found)[axis];
                if (!component.is_number())
                {
                    errorOut = std::string("field '") + key + "' must be an array of 3 numbers";
                    return false;
                }
                valueOut[axis] = static_cast<float>(component.get<double>());
            }
            return true;
        }

        /// @brief Reads the mandatory process kind a command is aimed at.
        /// @param object object to read from
        /// @param valueOut receives the kind
        /// @param errorOut receives the reason on failure
        /// @return true when the field names a known kind
        bool readTarget(const Json &object, StreamSource &valueOut, std::string &errorOut)
        {
            const auto found = object.find("target");
            if (found == object.end() || !found->is_string())
            {
                errorOut = "field 'target' must be a process kind name";
                return false;
            }
            const auto name = found->get<std::string>();
            for (const StreamSource kind : {StreamSource::FIRMWARE,
                                            StreamSource::DRONE_SIM,
                                            StreamSource::DRONE_REPLAY,
                                            StreamSource::SIM_PLANT})
            {
                if (name == streamSourceName(kind))
                {
                    valueOut = kind;
                    return true;
                }
            }
            errorOut = "unknown target '" + name + "'";
            return false;
        }

        /// @brief Reads the optional correlation id of a request.
        /// @param object object to read from
        /// @param valueOut receives the id, left at -1 when absent
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent or an integer
        bool readId(const Json &object, int &valueOut, std::string &errorOut)
        {
            const auto found = object.find("id");
            if (found == object.end())
            {
                return true;
            }
            if (!found->is_number_integer())
            {
                errorOut = "field 'id' must be an integer";
                return false;
            }
            valueOut = found->get<int>();
            return true;
        }

        /// @brief Fills the RC part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when every RC field decoded
        bool parseRc(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            message.rc.version = PROTOCOL_VERSION;
            message.rc.type = static_cast<std::uint8_t>(PacketType::RC_COMMAND);
            // The byte fields are read in place: a byte is aligned wherever
            // the packed layout puts it. The throttle is not, so it is read
            // into a local and stored afterwards.
            float throttle = message.rc.throttle;
            if (!readTarget(object, message.target, errorOut) ||
                !readByte(object, "kill", message.rc.killSwitch, errorOut) ||
                !readByte(object, "arm", message.rc.armSwitch, errorOut) ||
                !readByte(object, "mode", message.rc.mode, errorOut) ||
                !readFloat(object, "throttle", throttle, errorOut))
            {
                return false;
            }
            if (!(throttle >= 0.0f) || !(throttle <= 1.0f))
            {
                errorOut = "field 'throttle' must be in [0, 1]";
                return false;
            }
            message.rc.throttle = throttle;
            return true;
        }

        /// @brief Fills the scenario command part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when every field decoded
        bool parseSimCommand(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            const auto found = object.find("command");
            if (found == object.end() || !found->is_string())
            {
                errorOut = "field 'command' must be a scenario command name";
                return false;
            }
            const auto name = found->get<std::string>();
            if (name == "reset")
            {
                message.simCommand.command = SIM_COMMAND_RESET;
            }
            else if (name == "throw")
            {
                message.simCommand.command = SIM_COMMAND_THROW;
            }
            else if (name == "handThrow")
            {
                message.simCommand.command = SIM_COMMAND_HAND_THROW;
            }
            else
            {
                errorOut = "unknown command '" + name + "'";
                return false;
            }

            message.simCommand.version = PROTOCOL_VERSION;
            message.simCommand.type = static_cast<std::uint8_t>(PacketType::SIM_COMMAND);
            // Everything wider than a byte is decoded into aligned locals
            // first: the wire struct is packed, and nothing may hold a
            // reference to one of its fields.
            std::array<float, 3> velocity = readPackedField(&message.simCommand.velocityMps);
            std::array<float, 3> angular = readPackedField(&message.simCommand.angularVelocityRadS);
            float heldSeconds = message.simCommand.heldSeconds;
            float heldTiltRad = message.simCommand.heldTiltRad;
            float heldAzimuthRad = message.simCommand.heldAzimuthRad;
            float swingSeconds = message.simCommand.swingSeconds;
            if (!readVector(object, "velocityMps", velocity, errorOut) ||
                !readVector(object, "angularVelocityRadS", angular, errorOut) ||
                !readFloat(object, "heldSeconds", heldSeconds, errorOut) ||
                !readFloat(object, "heldTiltRad", heldTiltRad, errorOut) ||
                !readFloat(object, "heldAzimuthRad", heldAzimuthRad, errorOut) ||
                !readFloat(object, "swingSeconds", swingSeconds, errorOut))
            {
                return false;
            }
            writePackedField(&message.simCommand.velocityMps, velocity);
            writePackedField(&message.simCommand.angularVelocityRadS, angular);
            message.simCommand.heldSeconds = heldSeconds;
            message.simCommand.heldTiltRad = heldTiltRad;
            message.simCommand.heldAzimuthRad = heldAzimuthRad;
            message.simCommand.swingSeconds = swingSeconds;
            return true;
        }

        /// @brief Fills the recording part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when the action names start or stop
        bool parseRecord(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            const auto found = object.find("action");
            if (found == object.end() || !found->is_string())
            {
                errorOut = R"(field 'action' must be "start" or "stop")";
                return false;
            }
            const auto action = found->get<std::string>();
            if (action != "start" && action != "stop")
            {
                errorOut = R"(field 'action' must be "start" or "stop")";
                return false;
            }
            message.recordStart = action == "start";
            return true;
        }
    } // namespace

    std::string telemetryToJson(const TelemetryPacket &packet)
    {
        // Every field wider than a byte is copied out of the packed struct
        // before the JSON library takes a reference to it.
        const std::uint16_t sequence = packet.sequence;
        const std::uint64_t timestampUs = packet.timestampUs;
        const std::uint32_t throwCount = packet.throwCount;
        const std::uint64_t apexTimestampUs = packet.apexTimestampUs;

        Json message;
        message["type"] = "telemetry";
        message["sourceId"] = packet.sourceId;
        message["sequence"] = sequence;
        message["timestampUs"] = timestampUs;
        message["gyroRadS"] = floatsToJson(readPackedField(&packet.gyroRadS));
        message["attitudeQuat"] = floatsToJson(readPackedField(&packet.attitudeQuat));
        message["gyroBiasRadS"] = floatsToJson(readPackedField(&packet.gyroBiasRadS));
        message["motor"] = floatsToJson(readPackedField(&packet.motor));
        message["altitudeM"] = static_cast<double>(packet.altitudeM);
        message["verticalVelocityMps"] = static_cast<double>(packet.verticalVelocityMps);
        message["throwState"] = packet.throwState;
        message["throwCount"] = throwCount;
        message["releaseVelocityMps"] = static_cast<double>(packet.releaseVelocityMps);
        message["apexTimestampUs"] = apexTimestampUs;
        message["apexAltitudeM"] = static_cast<double>(packet.apexAltitudeM);
        message["flightPhase"] = packet.flightPhase;
        return message.dump();
    }

    std::string simRawToJson(const SimRawPacket &packet)
    {
        const std::uint16_t sequence = packet.sequence;
        const std::uint64_t timestampUs = packet.timestampUs;

        Json message;
        message["type"] = "simRaw";
        message["sourceId"] = packet.sourceId;
        message["sequence"] = sequence;
        message["timestampUs"] = timestampUs;
        message["attitudeQuat"] = floatsToJson(readPackedField(&packet.attitudeQuat));
        message["positionM"] = floatsToJson(readPackedField(&packet.positionM));
        message["velocityMps"] = floatsToJson(readPackedField(&packet.velocityMps));
        return message.dump();
    }

    std::string discoveryToJson(const std::vector<DiscoveredProcess> &processes,
                                std::uint64_t nowUs)
    {
        static constexpr std::uint64_t US_PER_MS = 1000U;
        Json entries = Json::array();
        for (const DiscoveredProcess &process : processes)
        {
            Json entry;
            entry["kind"] = static_cast<std::uint8_t>(process.kind);
            entry["kindName"] = streamSourceName(process.kind);
            entry["sessionId"] = process.sessionId;
            entry["telemetryPort"] = process.telemetryPort;
            entry["commandPort"] = process.commandPort;
            entry["viaSerial"] = process.viaSerial;
            entry["ageMs"] =
                (nowUs > process.lastSeenUs ? nowUs - process.lastSeenUs : 0U) / US_PER_MS;
            entries.push_back(entry);
        }
        Json message;
        message["type"] = "discovery";
        message["processes"] = entries;
        return message.dump();
    }

    std::string statusToJson(const HubStatus &status)
    {
        Json counts;
        counts["telemetryRows"] = status.telemetryRows;
        counts["simRawRows"] = status.simRawRows;
        counts["blackboxRecords"] = status.blackboxRecords;
        counts["badFrames"] = status.badFrames;
        counts["rejectedAnnounces"] = status.rejectedAnnounces;

        Json message;
        message["type"] = "status";
        message["recording"] = status.recording;
        message["serialOpen"] = status.serialOpen;
        message["counts"] = counts;
        message["clients"] = status.clients;
        return message.dump();
    }

    std::string ackToJson(int id, bool ok, std::string_view error)
    {
        Json message;
        message["type"] = "ack";
        message["id"] = id;
        message["ok"] = ok;
        message["error"] = std::string(error);
        return message.dump();
    }

    std::variant<ClientMessage, std::string> parseClientMessage(std::string_view text)
    {
        const Json root = Json::parse(text.begin(), text.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return std::string("message is not a JSON object");
        }
        const auto typeField = root.find("type");
        if (typeField == root.end() || !typeField->is_string())
        {
            return std::string("field 'type' must be a message type name");
        }

        ClientMessage message;
        std::string error;
        if (!readId(root, message.id, error))
        {
            return error;
        }

        const auto typeName = typeField->get<std::string>();
        if (typeName == "rc")
        {
            message.type = ClientMessageType::RC;
            if (!parseRc(root, message, error))
            {
                return error;
            }
        }
        else if (typeName == "simCommand")
        {
            message.type = ClientMessageType::SIM_COMMAND;
            if (!parseSimCommand(root, message, error))
            {
                return error;
            }
        }
        else if (typeName == "reboot")
        {
            message.type = ClientMessageType::REBOOT;
            if (!readTarget(root, message.target, error))
            {
                return error;
            }
            message.reboot.version = PROTOCOL_VERSION;
            message.reboot.type = static_cast<std::uint8_t>(PacketType::REBOOT_COMMAND);
            message.reboot.magic = BOARD_REBOOT_MAGIC;
        }
        else if (typeName == "record")
        {
            message.type = ClientMessageType::RECORD;
            if (!parseRecord(root, message, error))
            {
                return error;
            }
        }
        else
        {
            return "unknown message type '" + typeName + "'";
        }
        return message;
    }

    int clientMessageId(std::string_view text)
    {
        const Json root = Json::parse(text.begin(), text.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return -1;
        }
        const auto found = root.find("id");
        if (found == root.end() || !found->is_number_integer())
        {
            return -1;
        }
        return found->get<int>();
    }
} // namespace mark4
