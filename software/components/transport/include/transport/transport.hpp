#pragma once

/// @file
/// @brief The transport: an interface manager. The application declares its
///        physical links, then sends payloads to node ids; the transport
///        remembers on which link and at which address every node was last
///        heard, keeps them alive with a periodic keepalive and relays
///        frames between its links. It never reads a clock: every instant
///        comes from the caller.

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

        /// Keepalive cadence [us].
        static constexpr std::uint64_t KEEPALIVE_PERIOD_US = 1'000'000U;

        /// Silence after which a node is forgotten [us]: three missed keepalives.
        static constexpr std::uint64_t NODE_EXPIRY_US = 3'000'000U;

        /// Relays a frame may cross; a relay drops what already carries this many.
        static constexpr std::uint8_t MAX_HOPS = 4U;

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
            std::uint8_t hops = 0U; ///< relays the last frame from it crossed (0: direct neighbour)
            std::uint32_t boot = 0U; ///< boot id of the node's current incarnation, 0 until its
                                     ///< first keepalive
        };

        /// What crossed one link, in both directions, cumulative.
        struct LinkStats
        {
            std::uint32_t framesIn = 0U;  ///< frames the link handed over, header decoded or not
            std::uint32_t bytesIn = 0U;   ///< their bytes, header included
            std::uint32_t framesOut = 0U; ///< frames the link took: this node's sends,
                                          ///< its keepalives and what it relayed
            std::uint32_t bytesOut = 0U;  ///< their bytes, header included
            std::uint32_t refused = 0U;   ///< frames the link would not take (a full UART ring)
        };

        /// Receives one payload addressed to this node or to everyone.
        using DeliverFn = void (*)(void *context,
                                   std::uint32_t src,
                                   const std::uint8_t *payload,
                                   std::size_t size);

        /// @param nodeId identity of this node, never 0 (see node_id.hpp)
        /// @param bootId identity of this run of this node, drawn at random
        ///        by the composition and never derived from the chip, which
        ///        is what makes it differ from one boot to the next
        Transport(std::uint32_t nodeId, std::uint32_t bootId)
            : m_nodeId(nodeId),
              m_bootId(bootId)
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

        /// @brief Sends one payload: a broadcast leaves on every link, a
        ///        unicast on the link its destination was last heard on. An
        ///        application always sends a message: an empty payload is
        ///        refused. The frame is never flagged as a keepalive: that
        ///        flag is the transport's own.
        /// @param dst node to reach, BROADCAST_NODE for every node on every link
        /// @param payload payload bytes, never nullptr
        /// @param size payload size, 1 to MAX_PAYLOAD
        /// @return true when the frame left on a link (unicast: the node is
        ///         known and its link took the frame; broadcast: every link
        ///         did)
        bool send(std::uint32_t dst, const std::uint8_t *payload, std::size_t size);

        /// @brief Drains every link: learns nodes from every frame, delivers
        ///        what is for this node, relays the rest, expires the silent
        ///        nodes and emits the keepalive when due.
        /// @param nowUs current instant [us], from the caller's clock
        /// @param deliver receives every payload for this node
        /// @param context handed back to deliver, unchanged
        void poll(std::uint64_t nowUs, DeliverFn deliver, void *context);

        /// @return identity of this node
        [[nodiscard]] std::uint32_t nodeId() const
        {
            return m_nodeId;
        }

        /// @return identity of this run of this node, in every keepalive it
        ///         sends: a peer that sees it change knows this node rebooted
        [[nodiscard]] std::uint32_t bootId() const
        {
            return m_bootId;
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

        /// @return links declared
        [[nodiscard]] std::size_t linkCount() const
        {
            return m_linkCount;
        }

        /// @param index 0 <= index < linkCount()
        /// @return one declared link, in declaration order
        [[nodiscard]] const AbsLink &link(std::size_t index) const
        {
            return *m_links[index];
        }

        /// @param index 0 <= index < linkCount()
        /// @return what crossed that link since construction
        [[nodiscard]] const LinkStats &linkStats(std::size_t index) const
        {
            return m_linkStats[index];
        }

        /// @return peers forgotten for silence (NODE_EXPIRY_US), cumulative
        [[nodiscard]] std::uint32_t expired() const
        {
            return m_expired;
        }

        /// @return peers seen restarting (another boot id), cumulative
        [[nodiscard]] std::uint32_t restarted() const
        {
            return m_restarted;
        }

        /// @return frames dropped: shorter than a header, table full, or
        ///         addressed to nobody this node knows
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

        /// @return frames this node handed to a link: its own send() calls
        ///         and its keepalives
        [[nodiscard]] std::uint32_t sent() const
        {
            return m_sent;
        }

        /// @return payload bytes of the frames counted by sent(). A
        ///         keepalive counts none although it now carries a boot id:
        ///         what this counter describes is what the application
        ///         handed over, and the keepalive is the transport's own.
        [[nodiscard]] std::size_t sentBytes() const
        {
            return m_sentBytes;
        }

        /// @return send() calls that reached no link: an empty or too long
        ///         payload, no link declared, an unknown destination, or a
        ///         medium that refused the frame (a full UART ring)
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

        /// @brief Reads the boot id a keepalive carries and acts on it: the
        ///        first one is learnt silently, one that differs from a known
        ///        incarnation is a node that restarted, which leaves the
        ///        table and comes back into it (onNodeDown then onNodeUp).
        /// @param header header of the keepalive
        /// @param payload its payload, at least KEEPALIVE_PAYLOAD_SIZE bytes
        /// @param isNew true when learn() has just created the entry
        void onBootId(const FrameHeader &header, const std::uint8_t *payload, bool isNew);

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

        /// @brief Forwards the frame in m_rxBuffer with one hop more, or
        ///        drops it when it already crossed MAX_HOPS relays.
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

        /// @brief Emits one keepalive: this node's presence and the boot id
        ///        of this run behind a header flagged FRAME_FLAG_KEEPALIVE,
        ///        broadcast every KEEPALIVE_PERIOD_US and unicast once to a
        ///        node the moment it first appears. Counted like any send.
        /// @param dst node to reach, BROADCAST_NODE for every link
        void sendKeepalive(std::uint32_t dst);

        /// @brief Folds the outcome of one frame handed to one link into
        ///        that link's counters.
        /// @param linkIndex link the frame was handed to
        /// @param ok true when the link took it
        /// @param frameSize frame size, header included [bytes]
        /// @return ok, so a caller returns it straight away
        bool countLinkSend(std::size_t linkIndex, bool ok, std::size_t frameSize);

        /// @brief Folds the outcome of one send into the send-side counters.
        /// @param ok true when every link the frame was meant for took it
        /// @param size payload size of the frame [bytes], 0 for a keepalive
        /// @return ok, so a caller returns it straight away
        bool countSend(bool ok, std::size_t size);

        std::uint32_t m_nodeId;                         ///< this node
        std::uint32_t m_bootId;                         ///< this run of this node
        std::array<AbsLink *, MAX_LINKS> m_links{};     ///< declared links
        std::array<LinkStats, MAX_LINKS> m_linkStats{}; ///< what crossed each of them
        std::size_t m_linkCount = 0U;                   ///< links declared
        std::array<Node, MAX_NODES> m_nodes{};          ///< live nodes, dense prefix
        std::size_t m_nodeCount = 0U;                   ///< nodes in m_nodes
        std::uint16_t m_nextSeq = 0U;                   ///< sequence of the next frame sent
        std::uint64_t m_lastKeepaliveUs = 0U;           ///< instant of the last keepalive
        bool m_keepaliveSent = false;                   ///< true once one went out
        std::array<AbsPresenceListener *, MAX_LISTENERS> m_listeners{}; ///< attached, in order
        std::size_t m_listenerCount = 0U;                               ///< listeners attached
        bool m_listenersOverflow = false;                      ///< a fifth one tried to attach
        std::uint32_t m_dropped = 0U;                          ///< frames dropped
        std::uint32_t m_sent = 0U;                             ///< frames handed to a link
        std::size_t m_sentBytes = 0U;                          ///< payload bytes of those frames
        std::uint32_t m_refused = 0U;                          ///< sends that reached no link
        std::uint32_t m_relayed = 0U;                          ///< frames forwarded
        std::uint32_t m_expired = 0U;                          ///< peers forgotten for silence
        std::uint32_t m_restarted = 0U;                        ///< peers seen restarting
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
