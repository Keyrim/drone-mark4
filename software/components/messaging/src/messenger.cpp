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
    }

    void Messenger::poll(std::uint64_t nowUs)
    {
        m_nowUs = nowUs;
        m_transport.poll(nowUs, &Messenger::Deliver, this);
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
