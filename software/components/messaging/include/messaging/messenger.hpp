#pragma once

/// @file
/// @brief The postman and the sender: the one place an Envelope meets the
///        transport, in both directions. Inbound, every payload the
///        transport delivers is decoded once and handed to the one handler
///        that claimed its body tag; outbound, send() encodes one message on
///        the stack and unicasts it. No queue, no retry, no broadcast.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <pb.h>

#include "mark4.pb.h"
#include "transport/transport.hpp"

namespace mark4
{
    class AbsMessageHandler;

    /// Sees the raw bytes of every payload the transport delivers, before
    /// decoding. One at most: a gateway mirrors frames with it.
    using TapFn = void (*)(void *context,
                           std::uint32_t src,
                           const std::uint8_t *payload,
                           std::size_t size);

    /// The dispatch table and the encoder over one Transport. A composition
    /// that holds one calls poll() where it used to call the transport's, and
    /// send() where it used to encode.
    class Messenger
    {
      public:
        /// Slots of the dispatch table, indexed by body tag. Every tag of the
        /// Envelope oneof is below this.
        static constexpr std::size_t TAG_SLOTS = 64U;

        /// @param transport transport to poll and send on; must outlive the messenger
        explicit Messenger(Transport &transport)
            : m_transport(transport)
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
        ///        the tap, is decoded, and its handler is called.
        /// @param nowUs current instant [us], from the caller's clock
        void poll(std::uint64_t nowUs);

        /// @brief Encodes one message on the stack and unicasts it.
        /// @param dst node to reach; BROADCAST_NODE is refused
        /// @param envelope message to send; which_body must name a body
        /// @return true when the frame left on a link
        bool send(std::uint32_t dst, const mark4_Envelope &envelope);

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

        /// @return send() calls refused: a broadcast destination, an encoding
        ///         failure, or a transport that took nothing
        [[nodiscard]] std::uint32_t refused() const
        {
            return m_refused;
        }

      private:
        friend class AbsMessageHandler;

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
        std::array<AbsMessageHandler *, TAG_SLOTS> m_handlers{}; ///< one owner per tag
        std::uint32_t m_shadowed = 0U;    ///< claims that found their slot taken or out of range
        TapFn m_tap = nullptr;            ///< raw tap, none by default
        void *m_tapContext = nullptr;     ///< handed back to the tap
        std::uint64_t m_nowUs = 0U;       ///< instant of the poll in progress, handed to handlers
        std::uint32_t m_received = 0U;    ///< payloads delivered
        std::uint32_t m_undecodable = 0U; ///< payloads that were no Envelope
        std::uint32_t m_unhandled = 0U;   ///< messages nobody claimed
        std::uint32_t m_handled = 0U;     ///< messages acted on
        std::uint32_t m_ignored = 0U;     ///< messages their handler ignored
        std::uint32_t m_sent = 0U;        ///< sends that left
        std::uint32_t m_refused = 0U;     ///< sends that did not
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

      private:
        Messenger &m_messenger;            ///< where this handler is attached
        std::span<const pb_size_t> m_tags; ///< tags claimed, owned by the derived class
    };

    // A body tag past the slots is caught by init() at run time and, for the
    // highest tag of the schema today, here at compile time.
    static_assert(mark4_Envelope_identity_request_tag < Messenger::TAG_SLOTS,
                  "an Envelope body tag is past the dispatch table");
} // namespace mark4
