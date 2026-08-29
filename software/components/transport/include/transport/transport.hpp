#pragma once

/// @file
/// @brief The transport: an interface manager. The application declares its
///        physical links, then sends payloads to node ids; the transport
///        remembers on which link and at which address every node was last
///        heard, keeps them alive with a periodic beacon and, when asked,
///        relays frames between its links. It never reads a clock: every
///        instant comes from the caller.

#include <array>
#include <cstddef>
#include <cstdint>

#include "transport/frame.hpp"
#include "transport/link.hpp"

namespace mark4
{
    class Transport
    {
      public:
        /// Physical links one node may hold.
        static constexpr std::size_t MAX_LINKS = 4U;

        /// Nodes remembered at once; a frame from a further one is dropped.
        static constexpr std::size_t MAX_NODES = 32U;

        /// Largest beacon payload.
        static constexpr std::size_t MAX_BEACON_SIZE = 64U;

        /// Beacon cadence [us].
        static constexpr std::uint64_t BEACON_PERIOD_US = 1'000'000U;

        /// Silence after which a node is forgotten [us]: three missed beacons.
        static constexpr std::uint64_t NODE_EXPIRY_US = 3'000'000U;

        /// Relays a frame may cross before being dropped.
        static constexpr std::uint8_t INITIAL_HOPS = 4U;

        /// A forward jump of the sequence larger than this is a sender that
        /// restarted, not a burst of losses, and counts as nothing.
        static constexpr std::uint16_t RESYNC_THRESHOLD = 1024U;

        /// One node heard on one of the links.
        struct Node
        {
            std::uint32_t id = 0U;         ///< node id, never 0
            std::size_t link = 0U;         ///< index of the link it was last heard on
            LinkAddress address;           ///< where it is on that link
            std::uint64_t lastSeenUs = 0U; ///< instant of the last frame from it [us]
            std::uint16_t lastSeq = 0U;    ///< sequence of the last frame accepted
            std::uint32_t received = 0U;   ///< frames accepted from it
            std::uint32_t lost = 0U;       ///< frames the numbering says never arrived
            std::uint32_t duplicates = 0U; ///< frames carrying an already seen number
        };

        /// Receives one payload addressed to this node or to everyone.
        using DeliverFn = void (*)(void *context,
                                   std::uint32_t src,
                                   const std::uint8_t *payload,
                                   std::size_t size);

        /// Receives a node that just appeared or just expired.
        using NodeFn = void (*)(void *context, const Node &node);

        /// @param nodeId identity of this node, never 0 (see node_id.hpp)
        explicit Transport(std::uint32_t nodeId)
            : m_nodeId(nodeId)
        {
        }

        /// @brief Declares one physical link. Links are polled and broadcast
        ///        to in declaration order.
        /// @param link link, owned by the composition root
        /// @return false when MAX_LINKS are already declared
        bool addLink(AbsLink &link);

        /// @brief Checks the composition: a node id and at least one link.
        /// @return true when frames can flow
        [[nodiscard]] bool init() const;

        /// @brief Registers the payload broadcast every BEACON_PERIOD_US and
        ///        unicast to every node the moment it first appears. Copied.
        /// @param payload beacon bytes, at most MAX_BEACON_SIZE
        /// @param size beacon size, 0 to stop beaconing
        void setBeacon(const std::uint8_t *payload, std::size_t size);

        /// @brief Turns relaying on or off (off by default): a frame not for
        ///        this node is forwarded towards its destination.
        /// @param enabled true to relay
        void setRelay(bool enabled)
        {
            m_relay = enabled;
        }

        /// @brief Registers the presence callbacks.
        /// @param onUp called when a node is heard for the first time
        /// @param onDown called when a node has been silent for NODE_EXPIRY_US
        /// @param context handed back to both, unchanged
        void setNodeCallbacks(NodeFn onUp, NodeFn onDown, void *context)
        {
            m_onNodeUp = onUp;
            m_onNodeDown = onDown;
            m_nodeContext = context;
        }

        /// @brief Sends one payload.
        /// @param dst node to reach, BROADCAST_NODE for every node on every link
        /// @param payload payload bytes
        /// @param size payload size, at most MAX_PAYLOAD
        /// @return true when the frame left on a link (unicast: the node is
        ///         known and its link took the frame; broadcast: every link did)
        bool send(std::uint32_t dst, const std::uint8_t *payload, std::size_t size);

        /// @brief Drains every link: learns nodes from every frame, delivers
        ///        what is for this node, relays the rest, expires the silent
        ///        nodes and emits the beacon when due.
        /// @param nowUs current instant [us], from the caller's clock
        /// @param deliver receives every payload for this node
        /// @param context handed back to deliver, unchanged
        void poll(std::uint64_t nowUs, DeliverFn deliver, void *context);

        /// @return identity of this node
        [[nodiscard]] std::uint32_t nodeId() const
        {
            return m_nodeId;
        }

        /// @return true when the node has been heard within NODE_EXPIRY_US
        [[nodiscard]] bool isAlive(std::uint32_t nodeId) const
        {
            return findNode(nodeId) != nullptr;
        }

        /// @param nodeId node to look up
        /// @return the node, nullptr when unknown or expired
        [[nodiscard]] const Node *findNode(std::uint32_t nodeId) const;

        /// @return live nodes
        [[nodiscard]] std::size_t nodeCount() const
        {
            return m_nodeCount;
        }

        /// @param index 0 <= index < nodeCount()
        /// @return one live node
        [[nodiscard]] const Node &node(std::size_t index) const
        {
            return m_nodes[index];
        }

        /// @return frames dropped: shorter than a header, table full, or
        ///         addressed to nobody this node knows
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

      private:
        /// @brief Handles one frame read from one link.
        /// @param linkIndex link it arrived on
        /// @param from where it came from on that link
        /// @param size frame size, in m_rxBuffer
        /// @param nowUs current instant [us]
        /// @param deliver payload sink
        /// @param context handed back to deliver
        void onFrame(std::size_t linkIndex,
                     const LinkAddress &from,
                     std::size_t size,
                     std::uint64_t nowUs,
                     DeliverFn deliver,
                     void *context);

        /// @brief Refreshes or inserts the node a frame came from.
        /// @param header frame header
        /// @param linkIndex link it arrived on
        /// @param from address it came from
        /// @param nowUs current instant [us]
        /// @param[out] isNewOut true when the node was not known before
        /// @return false when the frame must be dropped (duplicate, table full)
        bool learn(const FrameHeader &header,
                   std::size_t linkIndex,
                   const LinkAddress &from,
                   std::uint64_t nowUs,
                   bool &isNewOut);

        /// @brief Forwards the frame in m_rxBuffer with one hop less.
        /// @param header its header
        /// @param arrivalLink link it must not go back on
        /// @param size frame size
        void relay(const FrameHeader &header, std::size_t arrivalLink, std::size_t size);

        /// @brief Forgets every node silent for NODE_EXPIRY_US.
        /// @param nowUs current instant [us]
        void expire(std::uint64_t nowUs);

        /// @param nodeId node to look up
        /// @return mutable node, nullptr when unknown
        Node *lookup(std::uint32_t nodeId);

        std::uint32_t m_nodeId;                                ///< this node
        std::array<AbsLink *, MAX_LINKS> m_links{};            ///< declared links
        std::size_t m_linkCount = 0U;                          ///< links declared
        std::array<Node, MAX_NODES> m_nodes{};                 ///< live nodes, dense prefix
        std::size_t m_nodeCount = 0U;                          ///< nodes in m_nodes
        std::uint16_t m_nextSeq = 0U;                          ///< sequence of the next frame sent
        bool m_relay = false;                                  ///< forward foreign frames
        std::array<std::uint8_t, MAX_BEACON_SIZE> m_beacon{};  ///< beacon payload
        std::size_t m_beaconSize = 0U;                         ///< 0 = no beacon
        std::uint64_t m_lastBeaconUs = 0U;                     ///< instant of the last beacon
        bool m_beaconSent = false;                             ///< true once one went out
        NodeFn m_onNodeUp = nullptr;                           ///< presence callback
        NodeFn m_onNodeDown = nullptr;                         ///< presence callback
        void *m_nodeContext = nullptr;                         ///< handed to both callbacks
        std::uint32_t m_dropped = 0U;                          ///< frames dropped
        std::array<std::uint8_t, MAX_FRAME_SIZE> m_rxBuffer{}; ///< frame being handled
        std::array<std::uint8_t, MAX_FRAME_SIZE> m_txBuffer{}; ///< frame being sent
    };
} // namespace mark4
