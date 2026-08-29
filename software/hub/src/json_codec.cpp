/// @file
/// @brief JSON codec implementation. Every conversion from a float field to
///        JSON goes through an explicit widening cast: the wire is
///        single-precision, JSON numbers are double, and the promotion must
///        be written down rather than happen behind our back.

#include "hub/json_codec.hpp"

#include <array>
#include <cstdlib>
#include <limits>
#include <nlohmann/json.hpp>

namespace mark4
{
    namespace
    {
        /// Insertion-ordered JSON, so a message reads in the order the wire
        /// message declares its fields instead of in alphabetical order.
        using Json = nlohmann::ordered_json;

        /// @brief Renders a fixed-size float array as a JSON array.
        /// @param values values to render
        /// @return JSON array of numbers
        template <std::size_t N> Json floatsToJson(const float (&values)[N])
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

        /// @brief Reads an optional boolean field, accepting the 0/1 the older
        ///        pages sent as well as true/false.
        /// @param object object to read from
        /// @param key field name
        /// @param valueOut receives the value, untouched when the field is absent
        /// @param errorOut receives the reason on failure
        /// @return true when the field is absent, a boolean or 0/1
        bool readFlag(const Json &object, const char *key, bool &valueOut, std::string &errorOut)
        {
            const auto found = object.find(key);
            if (found == object.end())
            {
                return true;
            }
            if (found->is_boolean())
            {
                valueOut = found->get<bool>();
                return true;
            }
            if (found->is_number_integer() &&
                (found->get<std::int64_t>() == 0 || found->get<std::int64_t>() == 1))
            {
                valueOut = found->get<std::int64_t>() == 1;
                return true;
            }
            errorOut = std::string("field '") + key + "' must be a boolean";
            return false;
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
        bool readParamId(const Json &object, std::uint32_t &valueOut, std::string &errorOut)
        {
            if (object.find("paramId") == object.end())
            {
                errorOut = "field 'paramId' must be a parameter id";
                return false;
            }
            std::uint16_t id = 0U;
            if (!readUnsigned(object, "paramId", id, errorOut))
            {
                return false;
            }
            valueOut = id;
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
                        float (&valueOut)[3],
                        std::string &errorOut)
        {
            const auto found = object.find(key);
            if (found == object.end())
            {
                return true;
            }
            if (!found->is_array() || found->size() != 3U)
            {
                errorOut = std::string("field '") + key + "' must be an array of 3 numbers";
                return false;
            }
            for (std::size_t axis = 0U; axis < 3U; ++axis)
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
        bool readTarget(const Json &object, mark4_NodeKind &valueOut, std::string &errorOut)
        {
            const auto found = object.find("target");
            if (found == object.end() || !found->is_string())
            {
                errorOut = "field 'target' must be a process kind name";
                return false;
            }
            const auto name = found->get<std::string>();
            if (parseNodeKindName(name, valueOut))
            {
                return true;
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
            message.command.which_body = mark4_Envelope_rc_tag;
            mark4_Rc &rc = message.command.body.rc;
            bool altitudeAuto = false;
            std::uint8_t mode = 0U;
            if (!readTarget(object, message.target, errorOut) ||
                !readFlag(object, "kill", rc.kill, errorOut) ||
                !readFlag(object, "arm", rc.arm, errorOut) ||
                !readUnsigned(object, "mode", mode, errorOut) ||
                !readFloat(object, "throttle", rc.throttle, errorOut))
            {
                return false;
            }
            altitudeAuto = mode == static_cast<std::uint8_t>(mark4_RcMode_RC_ALTITUDE_AUTO);
            rc.mode = altitudeAuto ? mark4_RcMode_RC_ALTITUDE_AUTO : mark4_RcMode_RC_MANUAL;
            if (!(rc.throttle >= 0.0f) || !(rc.throttle <= 1.0f))
            {
                errorOut = "field 'throttle' must be in [0, 1]";
                return false;
            }
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
            message.command.which_body = mark4_Envelope_sim_scenario_tag;
            mark4_SimScenario &scenario = message.command.body.sim_scenario;
            const auto name = found->get<std::string>();
            if (name == "reset")
            {
                scenario.kind = mark4_SimScenarioKind_RESET;
            }
            else if (name == "throw")
            {
                scenario.kind = mark4_SimScenarioKind_THROW;
            }
            else if (name == "handThrow")
            {
                scenario.kind = mark4_SimScenarioKind_HAND_THROW;
            }
            else
            {
                errorOut = "unknown scenario '" + name + "'";
                return false;
            }

            // A scenario goes to a flight process, which forwards it to the
            // plant it drives. The simulator is the only kind that has one,
            // so it is the default, but the field is honored when given.
            message.target = mark4_NodeKind_DRONE_SIM;
            if (object.contains("target") && !readTarget(object, message.target, errorOut))
            {
                return false;
            }

            std::uint8_t sequence = 0U;
            if (!readUnsigned(object, "sequence", sequence, errorOut) ||
                !readUnsigned(object, "seed", scenario.seed, errorOut) ||
                !readUnsigned(object, "throwDelayUs", scenario.throw_delay_us, errorOut) ||
                !readUnsigned(object, "hashWindowUs", scenario.hash_window_us, errorOut) ||
                !readVector(object, "velocityMps", scenario.velocity_mps, errorOut) ||
                !readVector(
                    object, "angularVelocityRadS", scenario.angular_velocity_rad_s, errorOut) ||
                !readFloat(object, "heldSeconds", scenario.held_seconds, errorOut) ||
                !readFloat(object, "heldTiltRad", scenario.held_tilt_rad, errorOut) ||
                !readFloat(object, "heldAzimuthRad", scenario.held_azimuth_rad, errorOut) ||
                !readFloat(object, "swingSeconds", scenario.swing_seconds, errorOut))
            {
                return false;
            }
            scenario.sequence = sequence;
            return true;
        }

        /// @brief Names one wire tuning status.
        /// @param status wire value
        /// @return static name, "unknown" outside the enumeration
        const char *tuningStatusName(mark4_TuningStatus status)
        {
            switch (status)
            {
                case mark4_TuningStatus_OK:
                    return "ok";
                case mark4_TuningStatus_UNKNOWN_ID:
                    return "unknownId";
                case mark4_TuningStatus_OUT_OF_BOUNDS:
                    return "outOfBounds";
                case mark4_TuningStatus_LOCKED_WHILE_ARMED:
                    return "lockedWhileArmed";
            }
            return "unknown";
        }

        /// @brief Fills the parameter write part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when every field decoded
        bool parseTuningSet(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            message.command.which_body = mark4_Envelope_tuning_set_tag;
            mark4_TuningSet &set = message.command.body.tuning_set;
            if (!readTarget(object, message.target, errorOut) ||
                !readParamId(object, set.id, errorOut))
            {
                return false;
            }
            if (object.find("value") == object.end())
            {
                errorOut = "field 'value' must be a number";
                return false;
            }
            return readFloat(object, "value", set.value, errorOut);
        }

        /// @brief Fills the table walk part of a client request.
        /// @param object message object
        /// @param message message being decoded
        /// @param errorOut receives the reason on failure
        /// @return true when every field decoded
        bool parseTuningList(const Json &object, ClientMessage &message, std::string &errorOut)
        {
            message.command.which_body = mark4_Envelope_tuning_list_tag;
            std::uint16_t startIndex = 0U;
            if (!readTarget(object, message.target, errorOut) ||
                !readUnsigned(object, "startIndex", startIndex, errorOut))
            {
                return false;
            }
            message.command.body.tuning_list.start_index = startIndex;
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
            message.target = mark4_NodeKind_FIRMWARE;
            if (object.find("target") == object.end())
            {
                return true;
            }
            return readTarget(object, message.target, errorOut);
        }

        /// @brief Renders the 8 hex characters of a wire hash.
        /// @param hash hash to render
        /// @return the hash, zero padded
        std::string wireHashText(std::uint32_t hash)
        {
            static constexpr std::size_t TEXT_CAPACITY = 9U;
            std::array<char, TEXT_CAPACITY> text{};
            static_cast<void>(std::snprintf(text.data(), text.size(), "%08x", hash));
            return text.data();
        }
    } // namespace

    std::string telemetryToJson(const mark4_Telemetry &telemetry, mark4_NodeKind source)
    {
        Json message;
        message["type"] = "telemetry";
        message["sourceId"] = static_cast<int>(source);
        message["timestampUs"] = telemetry.timestamp_us;
        message["gyroRadS"] = floatsToJson(telemetry.gyro_rad_s);
        message["attitudeQuat"] = floatsToJson(telemetry.attitude_quat);
        message["gyroBiasRadS"] = floatsToJson(telemetry.gyro_bias_rad_s);
        message["motor"] = floatsToJson(telemetry.motor);
        message["altitudeM"] = static_cast<double>(telemetry.altitude_m);
        message["baroAltitudeM"] = static_cast<double>(telemetry.baro_altitude_m);
        message["verticalVelocityMps"] = static_cast<double>(telemetry.vertical_velocity_mps);
        message["throwState"] = static_cast<int>(telemetry.throw_state);
        message["throwCount"] = telemetry.throw_count;
        message["releaseVelocityMps"] = static_cast<double>(telemetry.release_velocity_mps);
        message["apexTimestampUs"] = telemetry.apex_timestamp_us;
        message["apexAltitudeM"] = static_cast<double>(telemetry.apex_altitude_m);
        message["flightPhase"] = static_cast<int>(telemetry.flight_phase);
        return message.dump();
    }

    std::string simRawToJson(const mark4_Telemetry &telemetry, mark4_NodeKind source)
    {
        Json message;
        message["type"] = "simRaw";
        message["sourceId"] = static_cast<int>(source);
        message["timestampUs"] = telemetry.timestamp_us;
        message["attitudeQuat"] = floatsToJson(telemetry.truth.attitude_quat);
        message["positionM"] = floatsToJson(telemetry.truth.position_m);
        message["velocityMps"] = floatsToJson(telemetry.truth.velocity_mps);
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
            entry["kind"] = static_cast<int>(process.kind);
            entry["kindName"] = nodeKindName(process.kind);
            // The transport node id is what identifies one start of the
            // process, which is what this key has meant to the pages all along.
            entry["sessionId"] = process.nodeId;
            entry["name"] = process.name;
            entry["mcu"] = static_cast<int>(process.mcu);
            entry["buildEpoch"] = process.buildEpoch;
            entry["gitHash"] = process.gitHash;
            entry["wireHash"] = wireHashText(process.wireHash);
            entry["wireMismatch"] = process.wireMismatch;
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
            entry["ageMs"] =
                (nowUs > bridge.lastSeenUs ? nowUs - bridge.lastSeenUs : 0U) / US_PER_MS;
            found.push_back(entry);
        }
        Json message;
        message["type"] = "discovery";
        message["wireHash"] = wireHashText(WIRE_HASH);
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
            entry["stream"] = "transport";
            entry["sourceId"] = link.sourceId;
            entry["sourceName"] = link.sourceName;
            entry["received"] = link.received;
            entry["lost"] = link.lost;
            entry["duplicates"] = link.duplicates;
            entry["lossRate"] = linkLossRate(link);
            entry["lastSequence"] = link.lastSequence;
            links.push_back(entry);
        }

        // One drone at a time is THE connected drone: this is where every
        // page reads which one, and whether it currently shows signs of life.
        Json connection;
        connection["via"] = status.connectionVia.empty() ? "none" : status.connectionVia;
        connection["id"] = status.connectionId;
        connection["kind"] = static_cast<int>(status.connectionKind);
        connection["kindName"] = nodeKindName(status.connectionKind);
        connection["live"] = status.connectionLive;

        Json message;
        message["type"] = "status";
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
        bundleJson["wireHash"] = bundle.wireHash;
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
        boardJson["mcuId"] = static_cast<int>(board.mcu);
        boardJson["runningSlot"] = board.runningSlot;
        boardJson["activeSlot"] = board.activeSlot;
        Json slots = Json::array();
        for (const OtaSlotInfo &slot : board.slots)
        {
            Json entry;
            entry["state"] = static_cast<int>(slot.state);
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

    std::string tuningAckToJson(const mark4_TuningAck &ack, mark4_NodeKind source)
    {
        Json message;
        message["type"] = "tuningAck";
        message["source"] = nodeKindName(source);
        message["paramId"] = ack.id;
        message["value"] = static_cast<double>(ack.value);
        message["status"] = static_cast<int>(ack.status);
        message["statusName"] = tuningStatusName(ack.status);
        return message.dump();
    }

    std::string tuningInfoToJson(const mark4_TuningInfo &info, mark4_NodeKind source)
    {
        Json message;
        message["type"] = "tuningInfo";
        message["source"] = nodeKindName(source);
        message["index"] = info.index;
        message["count"] = info.count;
        message["paramId"] = info.id;
        message["name"] = info.name;
        message["value"] = static_cast<double>(info.value);
        message["minValue"] = static_cast<double>(info.min_value);
        message["maxValue"] = static_cast<double>(info.max_value);
        message["armedChange"] = info.armed_change;
        return message.dump();
    }

    std::string logToJson(const mark4_Log &log, mark4_NodeKind source)
    {
        Json message;
        message["type"] = "log";
        message["source"] = nodeKindName(source);
        message["timestampUs"] = log.timestamp_us;
        message["level"] = static_cast<int>(log.level);
        message["text"] = log.text;
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
            message.command.which_body = mark4_Envelope_reboot_tag;
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
