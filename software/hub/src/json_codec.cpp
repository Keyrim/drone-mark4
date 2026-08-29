/// @file
/// @brief JSON codec implementation. Every conversion from a float field to
///        JSON goes through an explicit widening cast: the wire is
///        single-precision, JSON numbers are double, and the promotion must
///        be written down rather than happen behind our back.

#include "hub/json_codec.hpp"

#include <cstdlib>
#include <limits>
#include <nlohmann/json.hpp>

#include "hub/packed_field.hpp"
#include "hub/serial_transport.hpp"

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

        /// @brief Reads an optional unsigned integer field of any width.
        /// @tparam T unsigned integer type the field lands in
        /// @param object object to read from
        /// @param key field name
        /// @param valueOut receives the value, untouched when the field is absent
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent or an integer that fits
        template <typename T>
        bool readUnsigned(const Json &object, const char *key, T &valueOut, std::string &errorOut)
        {
            static_assert(std::is_unsigned_v<T>);
            const auto found = object.find(key);
            if (found == object.end())
            {
                return true;
            }
            if (!found->is_number_unsigned())
            {
                errorOut = std::string("field '") + key + "' must be a non-negative integer";
                return false;
            }
            const auto raw = found->get<std::uint64_t>();
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
            {
                errorOut = std::string("field '") + key + "' is out of range";
                return false;
            }
            valueOut = static_cast<T>(raw);
            return true;
        }

        /// @brief Reads the mandatory parameter id of a tuning request. The
        ///        key is "paramId", never "id": "id" is the correlation id
        ///        every message may carry, and confusing the two would match
        ///        an answer to the wrong request.
        /// @param object object to read from
        /// @param valueOut receives the id
        /// @param errorOut receives the reason on failure
        /// @return true when the field is present and in range
        bool readParamId(const Json &object, std::uint16_t &valueOut, std::string &errorOut)
        {
            if (object.find("paramId") == object.end())
            {
                errorOut = "field 'paramId' must be a parameter id";
                return false;
            }
            return readUnsigned(object, "paramId", valueOut, errorOut);
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
            for (const StreamSource kind :
                 {StreamSource::FIRMWARE, StreamSource::DRONE_SIM, StreamSource::SIM_PLANT})
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
            if (!found->is_number_integer() || found->get<std::int64_t>() < 0 ||
                found->get<std::int64_t>() > std::numeric_limits<int>::max())
            {
                // An id outside the int range would come back wrapped in the
                // ack and never match the request that carried it
                errorOut = "field 'id' must be an integer fitting 31 bits";
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

        /// @brief Fills the scenario part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when every field decoded
        bool parseSimScenario(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            const auto found = object.find("scenario");
            if (found == object.end() || !found->is_string())
            {
                errorOut = "field 'scenario' must be a scenario name";
                return false;
            }
            const auto name = found->get<std::string>();
            if (name == "reset")
            {
                message.simScenario.scenario.scenario = SIM_SCENARIO_RESET;
            }
            else if (name == "throw")
            {
                message.simScenario.scenario.scenario = SIM_SCENARIO_THROW;
            }
            else if (name == "handThrow")
            {
                message.simScenario.scenario.scenario = SIM_SCENARIO_HAND_THROW;
            }
            else
            {
                errorOut = "unknown scenario '" + name + "'";
                return false;
            }

            // A scenario goes to a flight process, which forwards it to the
            // plant it drives. The simulator is the only kind that has one,
            // so it is the default, but the field is honored when given.
            message.target = StreamSource::DRONE_SIM;
            if (object.contains("target") && !readTarget(object, message.target, errorOut))
            {
                return false;
            }

            message.simScenario.version = PROTOCOL_VERSION;
            message.simScenario.type = static_cast<std::uint8_t>(PacketType::SIM_SCENARIO);
            // Everything wider than a byte is decoded into aligned locals
            // first: the wire struct is packed, and nothing may hold a
            // reference to one of its fields.
            std::array<float, 3> velocity =
                readPackedField(&message.simScenario.scenario.velocityMps);
            std::array<float, 3> angular =
                readPackedField(&message.simScenario.scenario.angularVelocityRadS);
            std::uint64_t seed = 0U;
            std::uint32_t throwDelayUs = 0U;
            std::uint32_t hashWindowUs = 0U;
            float heldSeconds = 0.0f;
            float heldTiltRad = 0.0f;
            float heldAzimuthRad = 0.0f;
            float swingSeconds = 0.0f;
            if (!readUnsigned(
                    object, "sequence", message.simScenario.scenario.sequence, errorOut) ||
                !readUnsigned(object, "seed", seed, errorOut) ||
                !readUnsigned(object, "throwDelayUs", throwDelayUs, errorOut) ||
                !readUnsigned(object, "hashWindowUs", hashWindowUs, errorOut) ||
                !readVector(object, "velocityMps", velocity, errorOut) ||
                !readVector(object, "angularVelocityRadS", angular, errorOut) ||
                !readFloat(object, "heldSeconds", heldSeconds, errorOut) ||
                !readFloat(object, "heldTiltRad", heldTiltRad, errorOut) ||
                !readFloat(object, "heldAzimuthRad", heldAzimuthRad, errorOut) ||
                !readFloat(object, "swingSeconds", swingSeconds, errorOut))
            {
                return false;
            }
            writePackedField(&message.simScenario.scenario.velocityMps, velocity);
            writePackedField(&message.simScenario.scenario.angularVelocityRadS, angular);
            message.simScenario.scenario.seed = seed;
            message.simScenario.scenario.throwDelayUs = throwDelayUs;
            message.simScenario.scenario.hashWindowUs = hashWindowUs;
            message.simScenario.scenario.heldSeconds = heldSeconds;
            message.simScenario.scenario.heldTiltRad = heldTiltRad;
            message.simScenario.scenario.heldAzimuthRad = heldAzimuthRad;
            message.simScenario.scenario.swingSeconds = swingSeconds;
            return true;
        }

        /// @brief Names one wire-level tuning status.
        /// @param status one of the TUNING_ACK_* values
        /// @return static name, "unknown" outside the enumeration
        const char *tuningStatusName(std::uint8_t status)
        {
            switch (status)
            {
                case TUNING_ACK_OK:
                    return "ok";
                case TUNING_ACK_UNKNOWN_ID:
                    return "unknownId";
                case TUNING_ACK_OUT_OF_BOUNDS:
                    return "outOfBounds";
                case TUNING_ACK_LOCKED_WHILE_ARMED:
                    return "lockedWhileArmed";
                default:
                    return "unknown";
            }
        }

        /// @brief Reads a wire parameter name, which is zero-padded and
        ///        carries no terminator when it fills the field. strlen would
        ///        run off the end of that one, so the length is bounded here.
        /// @param name name field, already copied out of the packed struct
        /// @return the name, at most TUNING_NAME_SIZE characters
        std::string boundedName(const std::array<char, TUNING_NAME_SIZE> &name)
        {
            std::size_t length = 0U;
            while (length < name.size() && name[length] != '\0')
            {
                ++length;
            }
            return {name.data(), length};
        }

        /// @brief Fills the parameter write part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when every field decoded
        bool parseTuningSet(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            message.tuningSet.version = PROTOCOL_VERSION;
            message.tuningSet.type = static_cast<std::uint8_t>(PacketType::TUNING_SET);
            std::uint16_t id = 0U;
            float value = 0.0f;
            if (!readTarget(object, message.target, errorOut) || !readParamId(object, id, errorOut))
            {
                return false;
            }
            if (object.find("value") == object.end())
            {
                errorOut = "field 'value' must be a number";
                return false;
            }
            if (!readFloat(object, "value", value, errorOut))
            {
                return false;
            }
            message.tuningSet.id = id;
            message.tuningSet.value = value;
            return true;
        }

        /// @brief Fills the table walk part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when every field decoded
        bool parseTuningList(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            message.tuningList.version = PROTOCOL_VERSION;
            message.tuningList.type = static_cast<std::uint8_t>(PacketType::TUNING_LIST);
            std::uint16_t startIndex = 0U;
            if (!readTarget(object, message.target, errorOut) ||
                !readUnsigned(object, "startIndex", startIndex, errorOut))
            {
                return false;
            }
            message.tuningList.startIndex = startIndex;
            return true;
        }

        /// @brief Reads the mandatory profile name of a profile request.
        /// @param object object to read from
        /// @param valueOut receives the name
        /// @param errorOut receives the reason on failure
        /// @return true when the field names a usable profile
        bool readProfileName(const Json &object, std::string &valueOut, std::string &errorOut)
        {
            const auto found = object.find("name");
            if (found == object.end() || !found->is_string())
            {
                errorOut = "field 'name' must be a profile name";
                return false;
            }
            const auto name = found->get<std::string>();
            if (!TuningProfiles::ValidName(name))
            {
                // Names become file names, so the check belongs at the
                // boundary, before anything of the sort reaches the disk.
                errorOut = "invalid profile name '" + name + "'";
                return false;
            }
            valueOut = name;
            return true;
        }

        /// @brief Reads the values object of a profile save request: keys are
        ///        parameter ids as decimal strings, values are numbers.
        /// @param object object to read from
        /// @param valueOut receives the values
        /// @param errorOut receives the reason on failure
        /// @return true when every entry decoded
        bool readProfileValues(const Json &object, TuningValues &valueOut, std::string &errorOut)
        {
            static constexpr std::int64_t MAX_ID = 65535;
            // JSON object keys are strings, so a parameter id arrives as a
            // decimal one and is accumulated digit by digit.
            static constexpr std::int64_t ID_BASE = 10;
            const auto found = object.find("values");
            if (found == object.end() || !found->is_object())
            {
                errorOut = "field 'values' must be an object of paramId:value";
                return false;
            }
            for (const auto &entry : found->items())
            {
                const std::string &key = entry.key();
                if (key.empty() || !entry.value().is_number())
                {
                    errorOut = "field 'values' must be an object of paramId:value";
                    return false;
                }
                std::int64_t id = 0;
                for (const char character : key)
                {
                    if (character < '0' || character > '9')
                    {
                        errorOut = "parameter id '" + key + "' is not a decimal number";
                        return false;
                    }
                    id = id * ID_BASE + (character - '0');
                    if (id > MAX_ID)
                    {
                        errorOut = "parameter id '" + key + "' is out of range";
                        return false;
                    }
                }
                valueOut[static_cast<std::uint16_t>(id)] =
                    static_cast<float>(entry.value().get<double>());
            }
            return true;
        }

        /// @brief Fills the connect part of a client request. The route names
        ///        what identifies the drone: a UDP process is its kind, a
        ///        board is the bridge it is reached through, because the
        ///        board itself never announces.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when the route and its identity decoded
        bool parseConnect(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            const auto via = object.find("via");
            if (via == object.end() || !via->is_string())
            {
                errorOut = R"(field 'via' must be "udp" or "bridge")";
                return false;
            }
            message.connectVia = via->get<std::string>();
            if (message.connectVia == "udp")
            {
                return readTarget(object, message.target, errorOut);
            }
            if (message.connectVia == "bridge")
            {
                const auto name = object.find("name");
                if (name == object.end() || !name->is_string() || name->get<std::string>().empty())
                {
                    errorOut = "field 'name' must name the bridge to reach";
                    return false;
                }
                message.connectPeer = name->get<std::string>();
                return true;
            }
            errorOut = R"(field 'via' must be "udp" or "bridge")";
            return false;
        }

        /// @brief Fills the update-start part of a client request. The bundle
        ///        path is optional: the hub defaults it to the standard build
        ///        output, which is what the common case wants. Nothing is
        ///        checked here beyond the shape - the reader of the file says
        ///        whether it is a bundle, and says so in the operator's words.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent or a non-empty string
        bool parseOtaStart(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            const auto found = object.find("bundle");
            if (found == object.end() || found->is_null())
            {
                return true;
            }
            if (!found->is_string() || found->get<std::string>().empty())
            {
                errorOut = "field 'bundle' must be the path of an .ota bundle";
                return false;
            }
            message.otaBundlePath = found->get<std::string>();
            return true;
        }

        /// @brief Reads the optional process kind an update is aimed at. The
        ///        board is the default: it is the only thing that has flash.
        /// @param object object to read from
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent or names a known kind
        bool readOtaTarget(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            message.target = StreamSource::FIRMWARE;
            if (object.find("target") == object.end())
            {
                return true;
            }
            return readTarget(object, message.target, errorOut);
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
        message["baroAltitudeM"] = static_cast<double>(packet.baroAltitudeM);
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
                                const std::vector<DiscoveredBridge> &bridges,
                                std::uint64_t nowUs)
    {
        static constexpr std::uint64_t US_PER_MS = 1000U;
        Json entries = Json::array();
        for (const DiscoveredProcess &process : processes)
        {
            Json entry;
            entry["kind"] = static_cast<std::uint8_t>(process.kind);
            entry["kindName"] = streamSourceName(process.kind);
            // The transport node id is what identifies one start of the
            // process, which is what this key has meant to the pages all along.
            entry["sessionId"] = process.nodeId;
            entry["viaSerial"] = process.viaSerial;
            entry["ageMs"] =
                (nowUs > process.lastSeenUs ? nowUs - process.lastSeenUs : 0U) / US_PER_MS;
            entries.push_back(entry);
        }
        Json found = Json::array();
        for (const DiscoveredBridge &bridge : bridges)
        {
            Json entry;
            entry["address"] = bridge.address;
            entry["port"] = bridge.port;
            entry["name"] = bridge.name;
            entry["device"] = std::string(SerialTransport::UDP_PREFIX) + bridge.address + ":" +
                              std::to_string(bridge.port);
            entry["ageMs"] =
                (nowUs > bridge.lastSeenUs ? nowUs - bridge.lastSeenUs : 0U) / US_PER_MS;
            found.push_back(entry);
        }
        Json message;
        message["type"] = "discovery";
        message["processes"] = entries;
        message["bridges"] = found;
        return message.dump();
    }

    std::string statusToJson(const HubStatus &status)
    {
        Json counts;
        counts["badFrames"] = status.badFrames;
        counts["rejectedAnnounces"] = status.rejectedAnnounces;

        Json links = Json::array();
        for (const LinkHealth &link : status.links)
        {
            Json entry;
            entry["stream"] = streamKindName(link.stream);
            entry["sourceId"] = link.sourceId;
            entry["sourceName"] = link.sourceName;
            entry["received"] = link.received;
            entry["lost"] = link.lost;
            entry["duplicates"] = link.duplicates;
            entry["resyncs"] = link.resyncs;
            entry["lossRate"] = linkLossRate(link);
            entry["lastSequence"] = link.lastSequence;
            links.push_back(entry);
        }

        // One drone at a time is THE connected drone: this is where every
        // page reads which one, and whether it currently shows signs of life.
        Json connection;
        connection["via"] = status.connectionVia.empty() ? "none" : status.connectionVia;
        connection["id"] = status.connectionId;
        connection["kind"] = static_cast<std::uint8_t>(status.connectionKind);
        connection["kindName"] = streamSourceName(status.connectionKind);
        connection["live"] = status.connectionLive;

        Json message;
        message["type"] = "status";
        message["serialOpen"] = status.serialOpen;
        message["serialLink"] = status.serialLink;
        message["connection"] = connection;
        message["counts"] = counts;
        message["clients"] = status.clients;
        message["rcClients"] = status.rcClients;
        message["links"] = links;
        return message.dump();
    }

    std::string otaToJson(const OtaClient &client)
    {
        static constexpr double PERCENT = 100.0;
        const OtaBundle &bundle = client.bundle();
        const OtaBoardStatus &board = client.board();
        const OtaProgress &progress = client.progress();

        Json bundleJson;
        bundleJson["loaded"] = bundle.loaded();
        bundleJson["path"] = client.bundlePath();
        bundleJson["name"] = bundle.name;
        bundleJson["mcuId"] = bundle.mcuId;
        bundleJson["buildEpoch"] = bundle.buildEpoch;
        bundleJson["gitHash"] = bundle.gitHash;
        bundleJson["protocolVersion"] = bundle.protocolVersion;
        Json images = Json::array();
        for (const OtaBundleImage &image : bundle.images)
        {
            Json entry;
            entry["slot"] = image.slot;
            entry["size"] = image.size;
            entry["crc32"] = image.crc32;
            images.push_back(entry);
        }
        bundleJson["images"] = images;

        Json boardJson;
        boardJson["seen"] = board.seen;
        boardJson["mcuId"] = board.mcuId;
        boardJson["runningSlot"] = board.runningSlot;
        boardJson["activeSlot"] = board.activeSlot;
        Json slots = Json::array();
        for (const OtaSlotInfo &slot : board.slots)
        {
            Json entry;
            entry["state"] = slot.state;
            entry["stateName"] = otaSlotStateName(slot.state);
            entry["buildEpoch"] = slot.buildEpoch;
            entry["gitHash"] = slot.gitHash;
            slots.push_back(entry);
        }
        boardJson["slots"] = slots;
        boardJson["updaterBusy"] = board.updaterBusy;
        boardJson["buildEpoch"] = board.buildEpoch;
        boardJson["gitHash"] = board.gitHash;
        boardJson["slotSize"] = board.slotSize;
        boardJson["maxChunkData"] = board.maxChunkData;

        Json progressJson;
        progressJson["sentBytes"] = progress.sentBytes;
        progressJson["ackedBytes"] = progress.ackedBytes;
        progressJson["totalBytes"] = progress.totalBytes;
        progressJson["retries"] = progress.retries;
        // The percentage follows what the board has written, not what went
        // out: a bar that runs ahead of the flash would lie on every resend.
        progressJson["percent"] = progress.totalBytes == 0U
                                      ? 0.0
                                      : PERCENT * static_cast<double>(progress.ackedBytes) /
                                            static_cast<double>(progress.totalBytes);

        Json message;
        message["type"] = "ota";
        message["phase"] = otaPhaseName(client.phase());
        message["verdict"] = otaVerdictName(client.verdict());
        message["verdictText"] = client.verdictText();
        message["lastError"] = client.lastError();
        message["targetSlot"] =
            client.targetSlot() < OTA_SLOT_COUNT ? static_cast<int>(client.targetSlot()) : -1;
        message["bundle"] = bundleJson;
        message["board"] = boardJson;
        message["progress"] = progressJson;
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

    std::string tuningAckToJson(const TuningAckPacket &packet, StreamSource source)
    {
        // Copied out of the packed struct before the JSON library sees them.
        const std::uint16_t id = packet.id;
        const float value = packet.value;

        Json message;
        message["type"] = "tuningAck";
        message["source"] = streamSourceName(source);
        message["paramId"] = id;
        message["value"] = static_cast<double>(value);
        message["status"] = packet.status;
        message["statusName"] = tuningStatusName(packet.status);
        return message.dump();
    }

    std::string tuningInfoToJson(const TuningInfoPacket &packet, StreamSource source)
    {
        const std::uint16_t index = packet.index;
        const std::uint16_t count = packet.count;
        const std::uint16_t id = packet.id;
        const std::array<char, TUNING_NAME_SIZE> name = readPackedField(&packet.name);
        const float value = packet.value;
        const float minValue = packet.minValue;
        const float maxValue = packet.maxValue;

        Json message;
        message["type"] = "tuningInfo";
        message["source"] = streamSourceName(source);
        message["index"] = index;
        message["count"] = count;
        message["paramId"] = id;
        message["name"] = boundedName(name);
        message["value"] = static_cast<double>(value);
        message["minValue"] = static_cast<double>(minValue);
        message["maxValue"] = static_cast<double>(maxValue);
        message["armedChange"] = (packet.flags & TUNING_FLAG_ARMED_CHANGE) != 0U;
        return message.dump();
    }

    std::string profileNamesToJson(const std::vector<std::string> &names)
    {
        Json entries = Json::array();
        for (const std::string &name : names)
        {
            entries.push_back(name);
        }
        Json message;
        message["type"] = "profiles";
        message["names"] = entries;
        return message.dump();
    }

    std::string profileToJson(const std::string &name, const TuningValues &values)
    {
        Json entries = Json::object();
        for (const auto &[id, value] : values)
        {
            entries[std::to_string(id)] = static_cast<double>(value);
        }
        Json message;
        message["type"] = "profile";
        message["name"] = name;
        message["values"] = entries;
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
        else if (typeName == "simScenario")
        {
            message.type = ClientMessageType::SIM_SCENARIO;
            if (!parseSimScenario(root, message, error))
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
        else if (typeName == "tuningSet")
        {
            message.type = ClientMessageType::TUNING_SET;
            if (!parseTuningSet(root, message, error))
            {
                return error;
            }
        }
        else if (typeName == "tuningList")
        {
            message.type = ClientMessageType::TUNING_LIST;
            if (!parseTuningList(root, message, error))
            {
                return error;
            }
        }
        else if (typeName == "profileList")
        {
            message.type = ClientMessageType::PROFILE_LIST;
        }
        else if (typeName == "profileSave")
        {
            message.type = ClientMessageType::PROFILE_SAVE;
            if (!readProfileName(root, message.profileName, error) ||
                !readProfileValues(root, message.profileValues, error))
            {
                return error;
            }
        }
        else if (typeName == "profileLoad")
        {
            message.type = ClientMessageType::PROFILE_LOAD;
            if (!readProfileName(root, message.profileName, error))
            {
                return error;
            }
        }
        else if (typeName == "profilePush")
        {
            message.type = ClientMessageType::PROFILE_PUSH;
            if (!readProfileName(root, message.profileName, error) ||
                !readTarget(root, message.target, error))
            {
                return error;
            }
        }
        else if (typeName == "connect")
        {
            message.type = ClientMessageType::CONNECT;
            if (!parseConnect(root, message, error))
            {
                return error;
            }
        }
        else if (typeName == "disconnect")
        {
            message.type = ClientMessageType::DISCONNECT;
        }
        else if (typeName == "otaStatus")
        {
            message.type = ClientMessageType::OTA_STATUS;
            if (!readOtaTarget(root, message, error))
            {
                return error;
            }
        }
        else if (typeName == "otaStart")
        {
            message.type = ClientMessageType::OTA_START;
            if (!readOtaTarget(root, message, error) || !parseOtaStart(root, message, error))
            {
                return error;
            }
        }
        else if (typeName == "otaAbort")
        {
            message.type = ClientMessageType::OTA_ABORT;
        }
        else if (typeName == "otaRevert")
        {
            message.type = ClientMessageType::OTA_REVERT;
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
