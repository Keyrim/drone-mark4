#pragma once

/// @file
/// @brief The postman and the sender: the one place an Envelope meets the
///        transport, in both directions. Inbound, every payload the
///        transport delivers is decoded once and handed to the one handler
///        that claimed its body tag, and a message carrying a request id is
///        acknowledged before it is dispatched; outbound, send() encodes one
///        message on the stack and unicasts it once, request() numbers it,
///        keeps it and resends it until it is acknowledged. No broadcast.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <pb.h>

#include "protocol/envelope.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    class AbsMessageHandler;

    /// Silence after a send before the next one, unless the caller says
    /// otherwise [us].
    inline constexpr std::uint64_t DEFAULT_REQUEST_PERIOD_US = 500'000U;

    /// Sends of one request before it is given up on, the first one
    /// included, unless the caller says otherwise.
    inline constexpr std::uint8_t DEFAULT_REQUEST_RETRIES = 5U;

    /// How long one request is retried, and how often. Per call, because a
    /// reboot and a table page do not want the same values.
    struct RequestPolicy
    {
        std::uint64_t periodUs = DEFAULT_REQUEST_PERIOD_US; ///< silence before the next send [us]
        std::uint8_t retries = DEFAULT_REQUEST_RETRIES;     ///< sends before giving up
    };

    /// One request waiting for its acknowledgement, as the messenger keeps
    /// it: the encoded bytes and what the retry needs.
    struct PendingRequest
    {
        bool used = false;         ///< the entry holds a request
        std::uint32_t dst = 0U;    ///< node the request went to
        std::uint32_t id = 0U;     ///< request id, never 0 while used
        AbsMessageHandler *owner = ///< told when the request is given up on
            nullptr;
        RequestPolicy policy;      ///< how long this one is retried
        std::uint64_t sentUs = 0U; ///< instant of the last send [us]
        std::uint8_t sends = 0U;   ///< sends so far, the first one included
        std::size_t size = 0U;     ///< encoded bytes in bytes
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{}; ///< what is resent, as encoded
    };

    /// Sees the raw bytes of every payload the transport delivers, before
    /// decoding. One at most: a gateway mirrors frames with it.
    using TapFn = void (*)(void *context,
                           std::uint32_t src,
                           const std::uint8_t *payload,
                           std::size_t size);

    /// The dispatch table and the encoder over one Transport. A composition
    /// that holds one calls poll() where it used to call the transport's, and
    /// send() where it used to encode. It is also the transport's presence
    /// listener on the messaging side: it drops the pending requests of a
    /// node that goes down and relays both events to its handlers.
    class Messenger : public AbsPresenceListener
    {
      public:
        /// Slots of the dispatch table, indexed by body tag. Every tag of the
        /// Envelope oneof is below this.
        static constexpr std::size_t TAG_SLOTS = 64U;

        /// Pending requests a board affords: it talks to a handful of nodes
        /// and every entry costs one encoded Envelope.
        static constexpr std::size_t BOARD_PENDING_REQUESTS = 4U;

        /// Pending requests a gateway affords: one node appearing costs
        /// several requests at once, and it watches a whole LAN.
        static constexpr std::size_t HUB_PENDING_REQUESTS = 256U;

        /// @param transport transport to poll and send on; must outlive the messenger
        /// @param pending the table of requests waiting for their
        ///        acknowledgement, owned by the composition and declared
        ///        before the messenger: without a heap a fixed array knows
        ///        its size at compile time, and a span keeps one Messenger
        ///        type for every composition, the way a handler hands its
        ///        tags over
        Messenger(Transport &transport, std::span<PendingRequest> pending)
            : AbsPresenceListener(transport),
              m_transport(transport),
              m_pending(pending)
        {
        }

        /// @brief Checks the composition: no tag claimed twice, no tag out of
        ///        range. A handler whose tag was already taken (or is past
        ///        TAG_SLOTS) owns no slot for it and hears nothing on it, and
        ///        does not take the slot over when the owner goes away either:
        ///        a claim is made once, at construction. This reports such a
        ///        handler for as long as it lives.
        /// @return true when every live handler holds every tag it claimed
        [[nodiscard]] bool init() const
        {
            return m_shadowed == 0U;
        }

        /// @brief Installs the one raw tap, or removes it with nullptr.
        /// @param tap called with every delivered payload before it is decoded
        /// @param context handed back to tap, unchanged
        void setTap(TapFn tap, void *context)
        {
            m_tap = tap;
            m_tapContext = context;
        }

        /// @brief Polls the transport once; every delivered payload goes to
        ///        the tap, is decoded, acknowledged when it carries a request
        ///        id, and its handler is called. tick() runs at the end, so a
        ///        composition adds no call of its own.
        /// @param nowUs current instant [us], from the caller's clock
        void poll(std::uint64_t nowUs);

        /// @brief Resends what went unanswered and gives up on what is past
        ///        its policy. Called by poll(); public for a composition that
        ///        drives the retries on another cadence.
        /// @param nowUs current instant [us], from the caller's clock
        void tick(std::uint64_t nowUs);

        /// @brief Encodes one message on the stack and unicasts it, once:
        ///        the streams, which are only worth their own instant.
        /// @param dst node to reach; BROADCAST_NODE is refused
        /// @param envelope message to send; which_body must name a body
        /// @return true when the frame left on a link
        bool send(std::uint32_t dst, const mark4_Envelope &envelope);

        /// @brief A node appeared: every handler is told once.
        /// @param node the node, as the transport holds it
        void onNodeUp(const Transport::Node &node) override;

        /// @brief A node went down: its pending requests are given up on and
        ///        every handler is told once.
        /// @param node the node, as it was
        void onNodeDown(const Transport::Node &node) override;

        /// @return payloads the transport delivered
        [[nodiscard]] std::uint32_t received() const
        {
            return m_received;
        }

        /// @return payloads that were not a valid Envelope
        [[nodiscard]] std::uint32_t undecodable() const
        {
            return m_undecodable;
        }

        /// @return decoded messages with no handler for their tag
        [[nodiscard]] std::uint32_t unhandled() const
        {
            return m_unhandled;
        }

        /// @return messages whose handler returned true
        [[nodiscard]] std::uint32_t handled() const
        {
            return m_handled;
        }

        /// @return messages whose handler returned false
        [[nodiscard]] std::uint32_t ignored() const
        {
            return m_ignored;
        }

        /// @return send() calls whose frame left on a link
        [[nodiscard]] std::uint32_t sent() const
        {
            return m_sent;
        }

        /// @return send() and request() calls refused: a broadcast
        ///         destination, a destination the transport does not know
        ///         (request() only), a full pending table, an encoding
        ///         failure, or a transport that took nothing (send() only)
        [[nodiscard]] std::uint32_t refused() const
        {
            return m_refused;
        }

        /// @return requests started
        [[nodiscard]] std::uint32_t requests() const
        {
            return m_requests;
        }

        /// @return requests sent again because no acknowledgement came
        [[nodiscard]] std::uint32_t resent() const
        {
            return m_resent;
        }

        /// @return requests acknowledged by their destination
        [[nodiscard]] std::uint32_t completed() const
        {
            return m_completed;
        }

        /// @return requests given up on after their last send
        [[nodiscard]] std::uint32_t failed() const
        {
            return m_failed;
        }

        /// @return acknowledgements this node sent, one per numbered message
        ///         it received
        [[nodiscard]] std::uint32_t acked() const
        {
            return m_acked;
        }

        /// @return acknowledgements that matched no pending request: a
        ///         request already given up on, or one answered twice
        [[nodiscard]] std::uint32_t unmatchedAcks() const
        {
            return m_unmatchedAcks;
        }

      private:
        friend class AbsMessageHandler;

        /// @brief Numbers one message, unicasts it and keeps it until the
        ///        destination acknowledges it. Reached through
        ///        AbsMessageHandler::request(), which names the owner.
        /// @param dst node to reach; BROADCAST_NODE and a node the transport
        ///        does not know are refused, the caller acts on onNodeUp()
        /// @param envelope message to send; its request_id is written here
        /// @param owner handler told when the request is given up on
        /// @param policy how long this request is retried
        /// @return the request id taken, never 0; 0 when the request was
        ///         refused. A first send the transport refused (a full UART
        ///         ring) is kept and retried, so a non-zero id says nothing
        ///         about the frame having left
        std::uint32_t request(std::uint32_t dst,
                              mark4_Envelope &envelope,
                              AbsMessageHandler &owner,
                              RequestPolicy policy);

        /// @return the next request id of this node: a counter of its own,
        ///         never 0, so a request is the pair (node, id)
        std::uint32_t nextId();

        /// @brief Hands one encoded pending request to the transport.
        /// @param entry the request, used
        void emit(const PendingRequest &entry);

        /// @brief Sends one Ack carrying an id back to the node that asked.
        ///        A raw send of its own: an acknowledgement is not one of
        ///        this node's messages and does not count in sent().
        /// @param dst node that sent the request
        /// @param requestId id it carried
        void acknowledge(std::uint32_t dst, std::uint32_t requestId);

        /// @brief Completes the pending request one Ack answers.
        /// @param src node the acknowledgement came from
        /// @param requestId id it carries
        void onAck(std::uint32_t src, std::uint32_t requestId);

        /// @brief Calls one method on every distinct handler of the table,
        ///        once each, in no guaranteed order.
        /// @param nodeId node the event is about
        /// @param up true for onNodeUp(), false for onNodeDown()
        void notifyHandlers(std::uint32_t nodeId, bool up);

        /// @brief Gives the handler every free slot among its tags; a taken
        ///        or out-of-range tag counts as a failed claim. Called by the
        ///        handler's constructor.
        /// @param handler handler to attach
        void attach(AbsMessageHandler &handler);

        /// @brief Frees every slot the handler holds and releases its failed
        ///        claims. Called by the handler's destructor.
        /// @param handler handler to detach
        void detach(AbsMessageHandler &handler);

        /// @brief Transport::DeliverFn adapter.
        /// @param context the Messenger
        /// @param src node the payload came from
        /// @param payload payload bytes
        /// @param size payload size in bytes
        static void Deliver(void *context,
                            std::uint32_t src,
                            const std::uint8_t *payload,
                            std::size_t size);

        /// @brief Taps, decodes and dispatches one payload.
        /// @param src node the payload came from
        /// @param payload payload bytes
        /// @param size payload size in bytes
        void onPayload(std::uint32_t src, const std::uint8_t *payload, std::size_t size);

        Transport &m_transport;                                  ///< the wire
        std::span<PendingRequest> m_pending;                     ///< requests waiting for an Ack
        std::array<AbsMessageHandler *, TAG_SLOTS> m_handlers{}; ///< one owner per tag
        std::uint32_t m_shadowed = 0U;      ///< claims that found their slot taken or out of range
        TapFn m_tap = nullptr;              ///< raw tap, none by default
        void *m_tapContext = nullptr;       ///< handed back to the tap
        std::uint64_t m_nowUs = 0U;         ///< instant of the poll in progress, handed to handlers
        std::uint32_t m_received = 0U;      ///< payloads delivered
        std::uint32_t m_undecodable = 0U;   ///< payloads that were no Envelope
        std::uint32_t m_unhandled = 0U;     ///< messages nobody claimed
        std::uint32_t m_handled = 0U;       ///< messages acted on
        std::uint32_t m_ignored = 0U;       ///< messages their handler ignored
        std::uint32_t m_sent = 0U;          ///< sends that left
        std::uint32_t m_refused = 0U;       ///< sends that did not
        std::uint32_t m_nextId = 0U;        ///< id of the last request started
        std::uint32_t m_requests = 0U;      ///< requests started
        std::uint32_t m_resent = 0U;        ///< requests sent again
        std::uint32_t m_completed = 0U;     ///< requests acknowledged
        std::uint32_t m_failed = 0U;        ///< requests given up on
        std::uint32_t m_acked = 0U;         ///< acknowledgements this node sent
        std::uint32_t m_unmatchedAcks = 0U; ///< acknowledgements matching no pending request
    };

    /// Consumer of the messages of some body tags. Attaches to the messenger
    /// in its constructor and detaches in its destructor: declaring one as a
    /// member after the messenger is the whole wiring. Both bodies are inline
    /// like every other abstract class of the components: the library is
    /// built without RTTI and an out-of-line destructor would leave the
    /// typeinfo the RTTI-enabled executables reference undefined.
    class AbsMessageHandler
    {
      public:
        /// @param messenger messenger to attach to; must outlive the handler
        /// @param tags the Envelope body tags (which_body values) this handler
        ///        consumes. The span is stored, not copied, and read again when
        ///        the handler detaches, so it must outlive the handler: the
        ///        derived class holds a `static constexpr std::array<pb_size_t,
        ///        N> TAGS` and passes it. It travels through the constructor
        ///        because a virtual call there would run before the derived
        ///        vtable exists.
        explicit AbsMessageHandler(Messenger &messenger, std::span<const pb_size_t> tags)
            : m_messenger(messenger),
              m_tags(tags)
        {
            m_messenger.attach(*this);
        }

        virtual ~AbsMessageHandler()
        {
            m_messenger.detach(*this);
        }

        AbsMessageHandler(const AbsMessageHandler &) = delete;
        AbsMessageHandler &operator=(const AbsMessageHandler &) = delete;

        /// @return the Envelope body tags this handler consumes
        [[nodiscard]] std::span<const pb_size_t> tags() const
        {
            return m_tags;
        }

        /// @brief One decoded message of one of those tags.
        /// @param src node it came from: where an answer goes
        /// @param envelope the message
        /// @param nowUs instant of the poll that delivered it [us]
        /// @return true when the handler acted on it, false when it ignored it
        virtual bool onMessage(std::uint32_t src,
                               const mark4_Envelope &envelope,
                               std::uint64_t nowUs) = 0;

        /// @brief One of this handler's requests was never acknowledged and
        ///        is given up on. Does nothing by default.
        /// @param dst node it went to
        /// @param requestId id it carried
        virtual void onRequestFailed(std::uint32_t dst, std::uint32_t requestId)
        {
            static_cast<void>(dst);
            static_cast<void>(requestId);
        }

        /// @brief A node appeared on the transport. Does nothing by default.
        /// @param nodeId the node
        virtual void onNodeUp(std::uint32_t nodeId)
        {
            static_cast<void>(nodeId);
        }

        /// @brief A node went down: everything this handler knew of it is
        ///        stale. Does nothing by default.
        /// @param nodeId the node
        virtual void onNodeDown(std::uint32_t nodeId)
        {
            static_cast<void>(nodeId);
        }

      protected:
        /// @brief Sends one message as a request of this handler's: numbered,
        ///        kept and resent until the destination acknowledges it, then
        ///        given up on with onRequestFailed().
        /// @param dst node to reach
        /// @param envelope message to send; its request_id is written here
        /// @param policy how long this one is retried
        /// @return the request id taken, never 0; 0 when the request was
        ///         refused. Keep it where onRequestFailed() has something to
        ///         decide, drop it where it has not.
        std::uint32_t request(std::uint32_t dst,
                              mark4_Envelope &envelope,
                              RequestPolicy policy = RequestPolicy{})
        {
            return m_messenger.request(dst, envelope, *this, policy);
        }

        /// @return the messenger this handler is attached to, for one that
        ///         sends too
        Messenger &accessMessenger()
        {
            return m_messenger;
        }

      private:
        Messenger &m_messenger;            ///< where this handler is attached
        std::span<const pb_size_t> m_tags; ///< tags claimed, owned by the derived class
    };

    // A body tag past the slots is caught by init() at run time and, for the
    // highest tag of the schema today, here at compile time.
    static_assert(mark4_Envelope_telemetry_config_tag < Messenger::TAG_SLOTS,
                  "an Envelope body tag is past the dispatch table");
} // namespace mark4
