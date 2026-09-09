#pragma once

/// @file
/// @brief The transport: an interface manager. The application declares its
///        physical links, then sends payloads to node ids; the transport
///        remembers on which link and at which address every node was last
///        heard, keeps them alive with a periodic beacon and relays frames
///        between its links. It never reads a clock: every instant comes
///        from the caller.

#include <array>
#include <cstddef>
#include <cstdint>

#include "transport/frame.hpp"
#include "transport/link.hpp"

namespace mark4
{
    class AbsPresenceListener;

    class Transport
    {
      public:
        /// Physical links one node may hold.
        static constexpr std::size_t MAX_LINKS = 4U;

        /// Presence listeners one node may hold; init() fails past that.
        static constexpr std::size_t MAX_LISTENERS = 4U;

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

        /// @brief Checks the composition: a node id, at least one link and
        ///        at most MAX_LISTENERS presence listeners.
        /// @return true when frames can flow
        [[nodiscard]] bool init() const;

        /// @brief Registers the payload broadcast every BEACON_PERIOD_US and
        ///        unicast to every node the moment it first appears. Copied.
        /// @param payload beacon bytes, at most MAX_BEACON_SIZE
        /// @param size beacon size, 0 to stop beaconing
        void setBeacon(const std::uint8_t *payload, std::size_t size);

        /// @brief Sends one payload: a broadcast leaves on every link, a
        ///        unicast on the link its destination was last heard on.
        /// @param dst node to reach, BROADCAST_NODE for every node on every link
        /// @param payload payload bytes
        /// @param size payload size, at most MAX_PAYLOAD
        /// @return true when the frame left on a link (unicast: the node is
        ///         known and its link took the frame; broadcast: every link
        ///         did)
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

        /// @return frames this node's own send() handed to a link, the
        ///         periodic beacon included
        [[nodiscard]] std::uint32_t sent() const
        {
            return m_sent;
        }

        /// @return payload bytes of the frames counted by sent()
        [[nodiscard]] std::size_t sentBytes() const
        {
            return m_sentBytes;
        }

        /// @return send() calls that reached no link: a payload too long, no
        ///         link declared, an unknown destination, or a medium that
        ///         refused the frame (a full UART ring)
        [[nodiscard]] std::uint32_t refused() const
        {
            return m_refused;
        }

        /// @return frames forwarded onto another link (one per link for a
        ///         broadcast)
        [[nodiscard]] std::uint32_t relayed() const
        {
            return m_relayed;
        }

      private:
        friend class AbsPresenceListener;

        /// @brief Adds one listener, told of every node that appears or
        ///        expires from now on. Called by the listener's constructor.
        ///        Past MAX_LISTENERS the listener is not added and init()
        ///        fails.
        /// @param listener listener to add
        void attach(AbsPresenceListener &listener);

        /// @brief Removes one listener, if present. Called by the listener's
        ///        destructor.
        /// @param listener listener to remove
        void detach(AbsPresenceListener &listener);

        /// @brief Tells every listener a node appeared.
        /// @param node the node
        void notifyUp(const Node &node);

        /// @brief Tells every listener a node expired.
        /// @param node the node, as it was
        void notifyDown(const Node &node);

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

        /// @brief Folds the outcome of one send into the send-side counters.
        /// @param ok true when every link the frame was meant for took it
        /// @param size payload size of the frame [bytes]
        /// @return ok, so a caller returns it straight away
        bool countSend(bool ok, std::size_t size);

        std::uint32_t m_nodeId;                               ///< this node
        std::array<AbsLink *, MAX_LINKS> m_links{};           ///< declared links
        std::size_t m_linkCount = 0U;                         ///< links declared
        std::array<Node, MAX_NODES> m_nodes{};                ///< live nodes, dense prefix
        std::size_t m_nodeCount = 0U;                         ///< nodes in m_nodes
        std::uint16_t m_nextSeq = 0U;                         ///< sequence of the next frame sent
        std::array<std::uint8_t, MAX_BEACON_SIZE> m_beacon{}; ///< beacon payload
        std::size_t m_beaconSize = 0U;                        ///< 0 = no beacon
        std::uint64_t m_lastBeaconUs = 0U;                    ///< instant of the last beacon
        bool m_beaconSent = false;                            ///< true once one went out
        std::array<AbsPresenceListener *, MAX_LISTENERS> m_listeners{}; ///< attached, in order
        std::size_t m_listenerCount = 0U;                               ///< listeners attached
        bool m_listenersOverflow = false;                      ///< a fifth one tried to attach
        std::uint32_t m_dropped = 0U;                          ///< frames dropped
        std::uint32_t m_sent = 0U;                             ///< frames handed to a link
        std::size_t m_sentBytes = 0U;                          ///< payload bytes of those frames
        std::uint32_t m_refused = 0U;                          ///< sends that reached no link
        std::uint32_t m_relayed = 0U;                          ///< frames forwarded
        std::array<std::uint8_t, MAX_FRAME_SIZE> m_rxBuffer{}; ///< frame being handled
        std::array<std::uint8_t, MAX_FRAME_SIZE> m_txBuffer{}; ///< frame being sent
    };

    /// Told when a node appears or expires. Attaches to the transport in its
    /// constructor and detaches in its destructor: declaring one as a member
    /// after the transport is the whole wiring. A listener may send() from
    /// inside its callbacks. Both bodies are inline like every other abstract
    /// class of the components: the transport library is built without RTTI
    /// and an out-of-line destructor would leave the typeinfo the RTTI-enabled
    /// executables reference undefined.
    class AbsPresenceListener
    {
      public:
        /// @param transport transport to listen to; must outlive the listener
        explicit AbsPresenceListener(Transport &transport)
            : m_transport(transport)
        {
            m_transport.attach(*this);
        }

        virtual ~AbsPresenceListener()
        {
            m_transport.detach(*this);
        }

        AbsPresenceListener(const AbsPresenceListener &) = delete;
        AbsPresenceListener &operator=(const AbsPresenceListener &) = delete;

        /// @brief A node was heard for the first time.
        /// @param node the node, as the table holds it
        virtual void onNodeUp(const Transport::Node &node) = 0;

        /// @brief A node was silent for NODE_EXPIRY_US and is forgotten.
        /// @param node the node, as it was before being forgotten
        virtual void onNodeDown(const Transport::Node &node) = 0;

      private:
        Transport &m_transport; ///< where this listener is attached
    };
} // namespace mark4
