#include "transport/transport.hpp"

#include <cstring>

namespace mark4
{
    bool Transport::addLink(AbsLink &link)
    {
        if (m_linkCount >= MAX_LINKS)
        {
            return false;
        }
        m_links[m_linkCount] = &link;
        ++m_linkCount;
        return true;
    }

    bool Transport::init() const
    {
        return m_nodeId != BROADCAST_NODE && m_linkCount > 0U && !m_listenersOverflow;
    }

    void Transport::attach(AbsPresenceListener &listener)
    {
        if (m_listenerCount >= MAX_LISTENERS)
        {
            m_listenersOverflow = true;
            return;
        }
        m_listeners[m_listenerCount] = &listener;
        ++m_listenerCount;
    }

    void Transport::detach(AbsPresenceListener &listener)
    {
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            if (m_listeners[index] == &listener)
            {
                // Shift the rest down: the ones that stay keep the order
                // they attached in.
                --m_listenerCount;
                for (std::size_t next = index; next < m_listenerCount; ++next)
                {
                    m_listeners[next] = m_listeners[next + 1U];
                }
                m_listeners[m_listenerCount] = nullptr;
                return;
            }
        }
    }

    void Transport::notifyUp(const Node &node)
    {
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            m_listeners[index]->onNodeUp(node);
        }
    }

    void Transport::notifyDown(const Node &node)
    {
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            m_listeners[index]->onNodeDown(node);
        }
    }

    bool Transport::send(std::uint32_t dst, const std::uint8_t *payload, std::size_t size)
    {
        if (payload == nullptr || size == 0U || size > MAX_PAYLOAD || m_linkCount == 0U)
        {
            ++m_refused;
            return false;
        }
        FrameHeader header;
        header.src = m_nodeId;
        header.dst = dst;
        header.seq = m_nextSeq;
        header.hops = 0U;
        header.keepalive = false;
        ++m_nextSeq;

        encodeFrameHeader(header, m_txBuffer.data());
        std::memcpy(m_txBuffer.data() + FRAME_HEADER_SIZE, payload, size);
        const std::size_t frameSize = FRAME_HEADER_SIZE + size;

        if (dst == BROADCAST_NODE)
        {
            bool all = true;
            for (std::size_t index = 0U; index < m_linkCount; ++index)
            {
                all = m_links[index]->broadcast(m_txBuffer.data(), frameSize) && all;
            }
            return countSend(all, size);
        }
        const Node *target = findNode(dst);
        if (target == nullptr)
        {
            ++m_dropped;
            ++m_refused;
            return false;
        }
        return countSend(m_links[target->link]->send(m_txBuffer.data(), frameSize, target->address),
                         size);
    }

    void Transport::sendKeepalive(std::uint32_t dst)
    {
        FrameHeader header;
        header.src = m_nodeId;
        header.dst = dst;
        header.seq = m_nextSeq;
        header.hops = 0U;
        header.keepalive = true;
        ++m_nextSeq;
        encodeFrameHeader(header, m_txBuffer.data());
        // The boot id of this run, little-endian: a peer that sees it change
        // knows this node restarted without ever leaving its table.
        for (std::size_t index = 0U; index < KEEPALIVE_PAYLOAD_SIZE; ++index)
        {
            m_txBuffer[FRAME_HEADER_SIZE + index] = static_cast<std::uint8_t>(
                m_bootId >> (static_cast<unsigned>(index) * FRAME_BYTE_BITS));
        }
        const std::size_t frameSize = FRAME_HEADER_SIZE + KEEPALIVE_PAYLOAD_SIZE;

        if (dst == BROADCAST_NODE)
        {
            bool all = true;
            for (std::size_t index = 0U; index < m_linkCount; ++index)
            {
                all = m_links[index]->broadcast(m_txBuffer.data(), frameSize) && all;
            }
            static_cast<void>(countSend(all, 0U));
            return;
        }
        const Node *target = findNode(dst);
        static_cast<void>(countSend(
            target != nullptr &&
                m_links[target->link]->send(m_txBuffer.data(), frameSize, target->address),
            0U));
    }

    bool Transport::countSend(bool ok, std::size_t size)
    {
        if (!ok)
        {
            ++m_refused;
            return false;
        }
        ++m_sent;
        m_sentBytes += size;
        return true;
    }

    void Transport::poll(std::uint64_t nowUs, DeliverFn deliver, void *context)
    {
        for (std::size_t index = 0U; index < m_linkCount; ++index)
        {
            for (;;)
            {
                LinkAddress from;
                const std::size_t size =
                    m_links[index]->receive(m_rxBuffer.data(), m_rxBuffer.size(), from);
                if (size == 0U)
                {
                    break;
                }
                onFrame(index, from, size, nowUs, deliver, context);
            }
        }
        expire(nowUs);
        if (!m_keepaliveSent || nowUs - m_lastKeepaliveUs >= KEEPALIVE_PERIOD_US)
        {
            m_lastKeepaliveUs = nowUs;
            m_keepaliveSent = true;
            sendKeepalive(BROADCAST_NODE);
        }
    }

    void Transport::onFrame(std::size_t linkIndex,
                            const LinkAddress &from,
                            std::size_t size,
                            std::uint64_t nowUs,
                            DeliverFn deliver,
                            void *context)
    {
        FrameHeader header;
        if (!decodeFrameHeader(m_rxBuffer.data(), size, header))
        {
            ++m_dropped;
            return;
        }
        if (header.src == m_nodeId || header.src == BROADCAST_NODE)
        {
            // A broadcast comes back to its sender on a shared medium; it
            // carries nothing this node does not know.
            return;
        }
        bool isNew = false;
        if (!learn(header, linkIndex, from, nowUs, isNew))
        {
            return;
        }

        const std::uint8_t *payload = m_rxBuffer.data() + FRAME_HEADER_SIZE;
        const std::size_t payloadSize = size - FRAME_HEADER_SIZE;
        if (header.keepalive && payloadSize >= KEEPALIVE_PAYLOAD_SIZE)
        {
            onBootId(header, payload, isNew);
        }
        if (isNew)
        {
            notifyUp(*findNode(header.src));
            // The newcomer learns this node at once instead of waiting for
            // the next periodic keepalive.
            sendKeepalive(header.src);
        }

        if (!header.keepalive && payloadSize > 0U &&
            (header.dst == m_nodeId || header.dst == BROADCAST_NODE))
        {
            // A flagged frame is the transport's own, whatever it carries,
            // and a frame without a payload has nothing for the application
            // either: both were learnt from above and stop here.
            if (deliver != nullptr)
            {
                deliver(context, header.src, payload, payloadSize);
            }
        }
        if (header.dst != m_nodeId)
        {
            // With one link this forwards nothing: a broadcast has no other
            // link to leave on and a unicast's destination sits on the
            // arrival link.
            relay(header, linkIndex, size);
        }
    }

    void Transport::onBootId(const FrameHeader &header, const std::uint8_t *payload, bool isNew)
    {
        std::uint32_t boot = 0U;
        for (std::size_t index = 0U; index < KEEPALIVE_PAYLOAD_SIZE; ++index)
        {
            boot |= static_cast<std::uint32_t>(payload[index])
                    << (static_cast<unsigned>(index) * FRAME_BYTE_BITS);
        }
        Node *node = lookup(header.src);
        if (node == nullptr)
        {
            return;
        }
        if (isNew || node->boot == 0U)
        {
            // The first keepalive of a node this transport had only heard
            // data frames from: its incarnation, learnt without an event.
            node->boot = boot;
            return;
        }
        if (boot == 0U || boot == node->boot)
        {
            return;
        }
        // Another incarnation of the same id: the node restarted without
        // ever leaving the table. Everything the listeners knew of it is
        // stale, so it leaves and comes back.
        const Node gone = *node;
        *node = Node{};
        node->id = gone.id;
        node->link = gone.link;
        node->address = gone.address;
        node->lastSeenUs = gone.lastSeenUs;
        node->lastSeq = header.seq;
        node->received = 1U;
        node->hops = header.hops;
        node->boot = boot;
        notifyDown(gone);
        notifyUp(*node);
    }

    bool Transport::learn(const FrameHeader &header,
                          std::size_t linkIndex,
                          const LinkAddress &from,
                          std::uint64_t nowUs,
                          bool &isNewOut)
    {
        isNewOut = false;
        Node *node = lookup(header.src);
        if (node == nullptr)
        {
            if (m_nodeCount >= MAX_NODES)
            {
                ++m_dropped;
                return false;
            }
            node = &m_nodes[m_nodeCount];
            ++m_nodeCount;
            *node = Node{};
            node->id = header.src;
            node->lastSeq = header.seq;
            node->received = 1U;
            isNewOut = true;
        }
        else
        {
            node->lastSeenUs = nowUs;
            const auto delta = static_cast<std::uint16_t>(header.seq - node->lastSeq);
            if (delta == 0U)
            {
                // The same frame again: a relay loop closing, a medium that
                // duplicates, or this node's own forwarding echoed back by
                // a shared medium. Either way it was already handled.
                ++node->duplicates;
                return false;
            }
            if (delta > 1U && delta < RESYNC_THRESHOLD)
            {
                node->lost += delta - 1U;
            }
            node->lastSeq = header.seq;
            ++node->received;
        }
        node->link = linkIndex;
        node->address = from;
        node->lastSeenUs = nowUs;
        node->hops = header.hops;
        return true;
    }

    void Transport::relay(const FrameHeader &header, std::size_t arrivalLink, std::size_t size)
    {
        if (header.hops >= MAX_HOPS)
        {
            // The frame has crossed as many relays as allowed.
            ++m_dropped;
            return;
        }
        // The last header byte is rebuilt rather than incremented in place:
        // the hop count shares it with the flags, which travel unchanged.
        FrameHeader forwarded = header;
        forwarded.hops = static_cast<std::uint8_t>(header.hops + 1U);
        encodeFrameHeader(forwarded, m_rxBuffer.data());
        if (header.dst == BROADCAST_NODE)
        {
            for (std::size_t index = 0U; index < m_linkCount; ++index)
            {
                if (index != arrivalLink)
                {
                    static_cast<void>(m_links[index]->broadcast(m_rxBuffer.data(), size));
                    ++m_relayed;
                }
            }
            return;
        }
        const Node *target = findNode(header.dst);
        if (target == nullptr || target->link == arrivalLink)
        {
            // Unknown, or last heard on the very link the frame came from
            // (split horizon): nothing this node can add.
            ++m_dropped;
            return;
        }
        static_cast<void>(m_links[target->link]->send(m_rxBuffer.data(), size, target->address));
        ++m_relayed;
    }

    void Transport::expire(std::uint64_t nowUs)
    {
        // Backwards, so removing by swapping in the last entry never skips
        // one.
        for (std::size_t index = m_nodeCount; index > 0U; --index)
        {
            Node &node = m_nodes[index - 1U];
            if (nowUs < node.lastSeenUs || nowUs - node.lastSeenUs < NODE_EXPIRY_US)
            {
                continue;
            }
            const Node gone = node;
            --m_nodeCount;
            node = m_nodes[m_nodeCount];
            m_nodes[m_nodeCount] = Node{};
            notifyDown(gone);
        }
    }

    Transport::Node *Transport::lookup(std::uint32_t nodeId)
    {
        for (std::size_t index = 0U; index < m_nodeCount; ++index)
        {
            if (m_nodes[index].id == nodeId)
            {
                return &m_nodes[index];
            }
        }
        return nullptr;
    }

    const Transport::Node *Transport::findNode(std::uint32_t nodeId) const
    {
        for (std::size_t index = 0U; index < m_nodeCount; ++index)
        {
            if (m_nodes[index].id == nodeId)
            {
                return &m_nodes[index];
            }
        }
        return nullptr;
    }
} // namespace mark4
