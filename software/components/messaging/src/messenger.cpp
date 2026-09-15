#include "messaging/messenger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "protocol/envelope.hpp"
#include "transport/frame.hpp"

namespace mark4
{
    void Messenger::attach(AbsMessageHandler &handler)
    {
        for (const pb_size_t tag : handler.tags())
        {
            if (tag < TAG_SLOTS && m_handlers[tag] == nullptr)
            {
                m_handlers[tag] = &handler;
            }
            else
            {
                ++m_shadowed;
            }
        }
    }

    void Messenger::detach(AbsMessageHandler &handler)
    {
        for (const pb_size_t tag : handler.tags())
        {
            if (tag < TAG_SLOTS && m_handlers[tag] == &handler)
            {
                m_handlers[tag] = nullptr;
            }
            else
            {
                // The slot was somebody else's, or the tag out of range: this
                // was one of its failed claims, released with it.
                --m_shadowed;
            }
        }
        // A handler that goes away leaves nobody to tell about its requests.
        for (PendingRequest &entry : m_pending)
        {
            if (entry.used && entry.owner == &handler)
            {
                entry.used = false;
            }
        }
    }

    void Messenger::poll(std::uint64_t nowUs)
    {
        m_nowUs = nowUs;
        m_transport.poll(nowUs, &Messenger::Deliver, this);
        tick(nowUs);
    }

    void Messenger::tick(std::uint64_t nowUs)
    {
        for (PendingRequest &entry : m_pending)
        {
            if (!entry.used || nowUs - entry.sentUs < entry.policy.periodUs)
            {
                continue;
            }
            if (entry.sends < entry.policy.retries)
            {
                ++entry.sends;
                entry.sentUs = nowUs;
                ++m_resent;
                emit(entry);
                continue;
            }
            // Out of sends: the destination is there for the transport and
            // deaf to this message. Its owner decides what that means.
            AbsMessageHandler *owner = entry.owner;
            const std::uint32_t dst = entry.dst;
            const std::uint32_t id = entry.id;
            entry.used = false;
            ++m_failed;
            if (owner != nullptr)
            {
                owner->onRequestFailed(dst, id);
            }
        }
    }

    bool Messenger::send(std::uint32_t dst, const mark4_Envelope &envelope)
    {
        if (dst == BROADCAST_NODE)
        {
            ++m_refused;
            return false;
        }
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        if (!encodeEnvelope(envelope, bytes.data(), bytes.size(), size))
        {
            ++m_refused;
            return false;
        }
        if (!m_transport.send(dst, bytes.data(), size))
        {
            ++m_refused;
            return false;
        }
        ++m_sent;
        return true;
    }

    bool Messenger::request(std::uint32_t dst,
                            mark4_Envelope &envelope,
                            AbsMessageHandler &owner,
                            RequestPolicy policy)
    {
        // A node the transport does not know is not asked anything: the
        // caller acts on onNodeUp() instead.
        if (dst == BROADCAST_NODE || m_transport.findNode(dst) == nullptr)
        {
            ++m_refused;
            return false;
        }
        PendingRequest *slot = nullptr;
        for (PendingRequest &entry : m_pending)
        {
            if (!entry.used)
            {
                slot = &entry;
                break;
            }
        }
        if (slot == nullptr)
        {
            ++m_refused;
            return false;
        }
        envelope.request_id = nextId();
        std::size_t size = 0U;
        if (!encodeEnvelope(envelope, slot->bytes.data(), slot->bytes.size(), size))
        {
            ++m_refused;
            return false;
        }
        slot->used = true;
        slot->dst = dst;
        slot->id = envelope.request_id;
        slot->owner = &owner;
        slot->policy = policy;
        slot->size = size;
        slot->sends = 1U;
        // The instant of the poll in progress, or of the last one when the
        // request is started outside a poll: the retry is paced in periods
        // of half a second, which that offset never crosses.
        slot->sentUs = m_nowUs;
        ++m_requests;
        // A first send the transport refuses (a full UART ring) is kept all
        // the same: the retry is exactly what covers it.
        emit(*slot);
        return true;
    }

    std::uint32_t Messenger::nextId()
    {
        // Never 0: 0 is what an Envelope carrying no request id reads as,
        // so the counter wraps back to 1.
        ++m_nextId;
        if (m_nextId == 0U)
        {
            m_nextId = 1U;
        }
        return m_nextId;
    }

    void Messenger::emit(const PendingRequest &entry)
    {
        if (m_transport.send(entry.dst, entry.bytes.data(), entry.size))
        {
            ++m_sent;
            return;
        }
        ++m_refused;
    }

    void Messenger::acknowledge(std::uint32_t dst, std::uint32_t requestId)
    {
        mark4_Envelope ack = mark4_Envelope_init_zero;
        ack.which_body = mark4_Envelope_ack_tag;
        ack.request_id = requestId;
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        if (!encodeEnvelope(ack, bytes.data(), bytes.size(), size))
        {
            return;
        }
        if (m_transport.send(dst, bytes.data(), size))
        {
            ++m_acked;
        }
    }

    void Messenger::onAck(std::uint32_t src, std::uint32_t requestId)
    {
        for (PendingRequest &entry : m_pending)
        {
            if (entry.used && entry.dst == src && entry.id == requestId)
            {
                entry.used = false;
                ++m_completed;
                return;
            }
        }
        ++m_unmatchedAcks;
    }

    void Messenger::onNodeUp(const Transport::Node &node)
    {
        notifyHandlers(node.id, true);
    }

    void Messenger::onNodeDown(const Transport::Node &node)
    {
        // Nothing addressed to a node that left can still arrive: its
        // requests are given up on at once, whatever their policy had left.
        for (PendingRequest &entry : m_pending)
        {
            if (!entry.used || entry.dst != node.id)
            {
                continue;
            }
            AbsMessageHandler *owner = entry.owner;
            const std::uint32_t id = entry.id;
            entry.used = false;
            ++m_failed;
            if (owner != nullptr)
            {
                owner->onRequestFailed(node.id, id);
            }
        }
        notifyHandlers(node.id, false);
    }

    void Messenger::notifyHandlers(std::uint32_t nodeId, bool up)
    {
        // One call per handler, not per tag: a handler claiming several tags
        // sits in several slots. The attach order decides nothing here.
        for (std::size_t slot = 0U; slot < TAG_SLOTS; ++slot)
        {
            AbsMessageHandler *handler = m_handlers[slot];
            if (handler == nullptr)
            {
                continue;
            }
            bool seen = false;
            for (std::size_t earlier = 0U; earlier < slot; ++earlier)
            {
                seen = seen || m_handlers[earlier] == handler;
            }
            if (seen)
            {
                continue;
            }
            if (up)
            {
                handler->onNodeUp(nodeId);
            }
            else
            {
                handler->onNodeDown(nodeId);
            }
        }
    }

    void Messenger::Deliver(void *context,
                            std::uint32_t src,
                            const std::uint8_t *payload,
                            std::size_t size)
    {
        static_cast<Messenger *>(context)->onPayload(src, payload, size);
    }

    void Messenger::onPayload(std::uint32_t src, const std::uint8_t *payload, std::size_t size)
    {
        ++m_received;
        if (m_tap != nullptr)
        {
            m_tap(m_tapContext, src, payload, size);
        }
        mark4_Envelope envelope;
        if (!decodeEnvelope(payload, size, envelope))
        {
            ++m_undecodable;
            return;
        }
        if (envelope.which_body == mark4_Envelope_ack_tag)
        {
            // The ack tag is the messenger's own: a handler that claims it is
            // shadowed like any duplicate claim, and init() reports it.
            onAck(src, envelope.request_id);
            return;
        }
        if (envelope.request_id != 0U)
        {
            // Acknowledged before dispatch, whether or not a handler claims
            // the tag: it says the message arrived, not what was done with
            // it, so a Reboot is acknowledged before the reset.
            acknowledge(src, envelope.request_id);
        }
        AbsMessageHandler *handler =
            envelope.which_body < TAG_SLOTS ? m_handlers[envelope.which_body] : nullptr;
        if (handler == nullptr)
        {
            ++m_unhandled;
            return;
        }
        if (handler->onMessage(src, envelope, m_nowUs))
        {
            ++m_handled;
        }
        else
        {
            ++m_ignored;
        }
    }
} // namespace mark4
