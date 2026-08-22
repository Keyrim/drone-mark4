#pragma once

/// @file
/// @brief Registry of the flight processes currently alive, fed by the
///        AnnouncePacket broadcast. Pure logic: no socket, no clock, no
///        thread. The caller passes the datagrams and the current time, so
///        the whole discovery behavior is reproducible in a unit test.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protocol/announce.hpp"
#include "protocol/header.hpp"

namespace mark4
{
    /// One process the ground side knows about, either because it announced
    /// itself over UDP or because its telemetry reached us over a serial link.
    struct DiscoveredProcess
    {
        StreamSource kind = StreamSource::FIRMWARE; ///< kind the process claims
        std::uint32_t sessionId = 0U;               ///< identity of the process start,
                                                    ///< 0 when the process assigns none
        std::uint16_t telemetryPort = 0U;           ///< port it broadcasts telemetry to,
                                                    ///< 0 when it has none
        std::uint16_t commandPort = 0U;             ///< port it listens on for commands,
                                                    ///< 0 when it has none
        std::uint64_t lastSeenUs = 0U;              ///< time of the last sign of life [us]
        bool viaSerial = false;                     ///< true when the evidence came from
                                                    ///< a serial link, not from an announce
    };

    /// What happened to one entry of the registry.
    enum class DiscoveryEvent : std::uint8_t
    {
        APPEARED,   ///< a kind and port pair nobody had seen yet
        RESTARTED,  ///< same kind and ports, new session identity
        DISAPPEARED ///< nothing heard for longer than the expiry delay
    };

    /// One event and the entry it concerns (a copy: DISAPPEARED entries are
    /// already gone from the registry when the caller reads them).
    struct DiscoveryChange
    {
        DiscoveryEvent event = DiscoveryEvent::APPEARED; ///< what happened
        DiscoveredProcess process;                       ///< entry the event concerns
    };

    /// @brief Human-readable name of a stream source, as the JSON messages
    ///        and the log lines spell it.
    /// @param kind stream source to name
    /// @return static name, "unknown" for a value outside the enumeration
    const char *streamSourceName(StreamSource kind);

    /// One WiFi bridge that told the network it is there. A bridge is not a
    /// process and carries no telemetry of its own: it is the address a
    /// serial link can be opened on.
    struct DiscoveredBridge
    {
        std::string address;           ///< where it announced from, dotted quad
        std::uint16_t port = 0U;       ///< port it carries the board stream on
        std::string name;              ///< what it calls itself
        std::uint64_t lastSeenUs = 0U; ///< time of the last announce [us]
    };

    /// Bridges heard on the bridge announce port. Pure logic, like the
    /// registry above: the caller passes the datagrams and the time.
    /// Nobody chooses the address of a bridge (a router hands it out), so
    /// this is what spares the operator from having to know it.
    class BridgeDirectory
    {
      public:
        /// Word every announce opens with, followed by a space and the name.
        static constexpr const char *WORD = "mark4-bridge";

        /// Longest name kept from an announce. A name comes from the network
        /// and reaches a web page: what is not a letter, a digit or a dash is
        /// dropped, and what is left is cut to this length.
        static constexpr std::size_t MAX_NAME = 16U;

        /// @brief Feeds one datagram received on the bridge announce port.
        /// @param address address the datagram came from, dotted quad
        /// @param port port the datagram came from, which is the port the
        ///        bridge carries the board stream on
        /// @param data datagram bytes
        /// @param size datagram size in bytes
        /// @param nowUs current time [us], the entry's freshness reference
        /// @return true when a bridge nobody had seen yet was added
        bool onAnnounce(const char *address,
                        std::uint16_t port,
                        const std::uint8_t *data,
                        std::size_t size,
                        std::uint64_t nowUs);

        /// @brief Drops the bridges nothing has been heard from for too long.
        /// @param nowUs current time [us]
        /// @param expiryUs silence after which a bridge is declared gone [us]
        /// @return number of bridges dropped
        std::size_t expire(std::uint64_t nowUs, std::uint64_t expiryUs);

        /// @return live bridges, in the order they were first seen
        [[nodiscard]] const std::vector<DiscoveredBridge> &bridges() const
        {
            return m_bridges;
        }

      private:
        std::vector<DiscoveredBridge> m_bridges; ///< live bridges, insertion order
    };

    /// Set of live processes, keyed by the (kind, telemetry port, command
    /// port) tuple: that tuple is what a consumer has to wire itself to, and
    /// the session identity behind it is what tells a restart from a refresh.
    class DiscoveryRegistry
    {
      public:
        /// @brief Feeds one datagram received on the announce port.
        /// @param data datagram bytes
        /// @param size datagram size in bytes
        /// @param nowUs current time [us], the entry's freshness reference
        /// @return the event this announce caused, nothing on a plain refresh
        ///         or on a datagram that is not a valid announce
        std::optional<DiscoveryChange> onAnnounce(const std::uint8_t *data,
                                                  std::size_t size,
                                                  std::uint64_t nowUs);

        /// @brief Records that firmware telemetry arrived over a serial link.
        ///        The board cannot broadcast UDP, so it never announces
        ///        itself: its telemetry is the only evidence it is alive.
        /// @param nowUs current time [us]
        /// @return APPEARED the first time, nothing afterwards
        std::optional<DiscoveryChange> onSerialTelemetry(std::uint64_t nowUs);

        /// @brief Drops the entries nothing has been heard from for too long.
        /// @param nowUs current time [us]
        /// @param expiryUs silence after which an entry is declared gone [us]
        /// @return one DISAPPEARED change per dropped entry
        std::vector<DiscoveryChange> expire(std::uint64_t nowUs, std::uint64_t expiryUs);

        /// @return live entries, in the order they were first seen
        [[nodiscard]] const std::vector<DiscoveredProcess> &processes() const
        {
            return m_processes;
        }

        /// @brief Looks up the command port of a live process of one kind.
        /// @param kind kind to look for
        /// @return command port, 0 when no live process of that kind serves one
        [[nodiscard]] std::uint16_t commandPortOf(StreamSource kind) const;

        /// @return announce datagrams dropped for a wrong version byte, a
        ///         wrong type byte or a wrong size
        [[nodiscard]] std::uint64_t rejectedAnnounces() const
        {
            return m_rejectedAnnounces;
        }

      private:
        /// @brief Refreshes or inserts an entry, deciding which event it is.
        /// @param candidate entry as the evidence describes it
        /// @return the event caused, nothing on a plain refresh
        std::optional<DiscoveryChange> touch(const DiscoveredProcess &candidate);

        std::vector<DiscoveredProcess> m_processes; ///< live entries, insertion order
        std::uint64_t m_rejectedAnnounces = 0U;     ///< announces dropped as invalid
    };
} // namespace mark4
