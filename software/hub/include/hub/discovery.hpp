#pragma once

/// @file
/// @brief Registry of the nodes currently alive, fed by the Announce every
///        node beacons through its transport frames, the board included.
///        Pure logic: no socket, no clock, no thread. The caller passes the
///        messages and the current time, so the whole discovery behavior is
///        reproducible in a unit test.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protocol/envelope.hpp"

namespace mark4
{
    /// One node the ground side knows about, as its last Announce described
    /// it.
    struct DiscoveredProcess
    {
        mark4_NodeKind kind = mark4_NodeKind_FIRMWARE; ///< kind the node claims
        std::uint32_t nodeId = 0U;                     ///< transport node the process is, the
                                                       ///< address commands go to
        std::uint64_t lastSeenUs = 0U;                 ///< time of the last announce [us]
        std::string name;                              ///< label the node gave itself
        mark4_Mcu mcu = mark4_Mcu_MCU_UNSPECIFIED;     ///< chip it runs on
        std::uint32_t buildEpoch = 0U;                 ///< build identity, 0 when none
        std::string gitHash;                           ///< short commit hash, empty when none
        std::uint32_t wireHash = 0U;                   ///< schema hash the node was built on
        bool wireMismatch = false;                     ///< its schema is not this hub's
    };

    /// What happened to one entry of the registry.
    enum class DiscoveryEvent : std::uint8_t
    {
        APPEARED,   ///< a kind nobody had seen yet
        RESTARTED,  ///< same kind, new node identity
        DISAPPEARED ///< nothing heard for longer than the expiry delay
    };

    /// One event and the entry it concerns (a copy: DISAPPEARED entries are
    /// already gone from the registry when the caller reads them).
    struct DiscoveryChange
    {
        DiscoveryEvent event = DiscoveryEvent::APPEARED; ///< what happened
        DiscoveredProcess process;                       ///< entry the event concerns
    };

    /// @brief Human-readable name of a node kind, as the JSON messages and
    ///        the log lines spell it.
    /// @param kind node kind to name
    /// @return static name, "unknown" for a value outside the enumeration
    const char *nodeKindName(mark4_NodeKind kind);

    /// @brief Parses a node kind name back, the inverse of nodeKindName().
    /// @param name name to look up
    /// @param[out] kindOut receives the kind when the name is known
    /// @return true when the name is a known kind
    bool parseNodeKindName(const std::string &name, mark4_NodeKind &kindOut);

    /// One WiFi bridge that told the network it is there. A bridge is not a
    /// node and carries no telemetry of its own: it is the address the
    /// board's link is opened on.
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

    /// Set of live nodes, keyed by kind: one drone_sim, one firmware. The
    /// node identity behind an entry is what tells a restart from a
    /// refresh, since a process draws a new one every time it starts.
    class DiscoveryRegistry
    {
      public:
        /// @brief Feeds one Announce.
        /// @param nodeId transport node the message came from
        /// @param announce decoded message
        /// @param nowUs current time [us], the entry's freshness reference
        /// @return the event this announce caused, nothing on a plain refresh
        ///         or on an announce naming no kind
        std::optional<DiscoveryChange> onAnnounce(std::uint32_t nodeId,
                                                  const mark4_Announce &announce,
                                                  std::uint64_t nowUs);

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

        /// @brief Looks up the transport node of a live process of one kind.
        /// @param kind kind to look for
        /// @return node id, 0 when no live process of that kind
        [[nodiscard]] std::uint32_t nodeIdOf(mark4_NodeKind kind) const;

        /// @brief Looks up the kind of a live transport node.
        /// @param nodeId transport node
        /// @param[out] kindOut receives the kind when the node is known
        /// @return true when the node announced itself
        [[nodiscard]] bool kindOf(std::uint32_t nodeId, mark4_NodeKind &kindOut) const;

        /// @return announces dropped for naming no kind
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
