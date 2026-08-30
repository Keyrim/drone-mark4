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
        return m_nodeId != BROADCAST_NODE && m_linkCount > 0U;
    }

    void Transport::setBeacon(const std::uint8_t *payload, std::size_t size)
    {
        if (payload == nullptr || size > MAX_BEACON_SIZE)
        {
            size = 0U;
        }
        if (size > 0U)
        {
            std::memcpy(m_beacon.data(), payload, size);
        }
        m_beaconSize = size;
        m_beaconSent = false;
    }

    bool Transport::send(std::uint32_t dst,
                         const std::uint8_t *payload,
                         std::size_t size,
                         std::uint32_t linkMask)
    {
        if (payload == nullptr || size > MAX_PAYLOAD || m_linkCount == 0U)
        {
            return false;
        }
        FrameHeader header;
        header.src = m_nodeId;
        header.dst = dst;
        header.seq = m_nextSeq;
        header.hops = INITIAL_HOPS;
        ++m_nextSeq;

        encodeFrameHeader(header, m_txBuffer.data());
        std::memcpy(m_txBuffer.data() + FRAME_HEADER_SIZE, payload, size);
        const std::size_t frameSize = FRAME_HEADER_SIZE + size;

        if (dst == BROADCAST_NODE)
        {
            bool all = true;
            for (std::size_t index = 0U; index < m_linkCount; ++index)
            {
                if ((linkMask & (1U << index)) != 0U)
                {
                    all = m_links[index]->broadcast(m_txBuffer.data(), frameSize) && all;
                }
            }
            return all;
        }
        const Node *target = findNode(dst);
        if (target == nullptr || (linkMask & (1U << target->link)) == 0U)
        {
            ++m_dropped;
            return false;
        }
        return m_links[target->link]->send(m_txBuffer.data(), frameSize, target->address);
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
        if (m_beaconSize > 0U && (!m_beaconSent || nowUs - m_lastBeaconUs >= BEACON_PERIOD_US))
        {
            m_lastBeaconUs = nowUs;
            m_beaconSent = true;
            static_cast<void>(send(BROADCAST_NODE, m_beacon.data(), m_beaconSize));
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
        if (isNew)
        {
            if (m_onNodeUp != nullptr)
            {
                m_onNodeUp(m_nodeContext, *findNode(header.src));
            }
            if (m_beaconSize > 0U)
            {
                // The newcomer learns this node at once instead of waiting
                // for the next periodic beacon.
                static_cast<void>(send(header.src, m_beacon.data(), m_beaconSize));
            }
        }

        const std::uint8_t *payload = m_rxBuffer.data() + FRAME_HEADER_SIZE;
        const std::size_t payloadSize = size - FRAME_HEADER_SIZE;
        if (header.dst == m_nodeId || header.dst == BROADCAST_NODE)
        {
            if (deliver != nullptr)
            {
                deliver(context, header.src, payload, payloadSize);
            }
        }
        if (header.dst != m_nodeId && m_relay)
        {
            relay(header, linkIndex, size);
        }
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
        return true;
    }

    void Transport::relay(const FrameHeader &header, std::size_t arrivalLink, std::size_t size)
    {
        if (header.hops <= 1U)
        {
            // Decrementing would reach 0: the frame has travelled far enough.
            ++m_dropped;
            return;
        }
        m_rxBuffer[FRAME_HEADER_SIZE - 1U] = static_cast<std::uint8_t>(header.hops - 1U);
        if (header.dst == BROADCAST_NODE)
        {
            for (std::size_t index = 0U; index < m_linkCount; ++index)
            {
                if (index != arrivalLink && relayAllowed(index, header, size))
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
        if (!relayAllowed(target->link, header, size))
        {
            return;
        }
        static_cast<void>(m_links[target->link]->send(m_rxBuffer.data(), size, target->address));
        ++m_relayed;
    }

    bool Transport::relayAllowed(std::size_t linkIndex, const FrameHeader &header, std::size_t size)
    {
        if (m_filter == nullptr || m_filter(m_filterContext,
                                            linkIndex,
                                            header,
                                            m_rxBuffer.data() + FRAME_HEADER_SIZE,
                                            size - FRAME_HEADER_SIZE))
        {
            return true;
        }
        ++m_filtered;
        return false;
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
            if (m_onNodeDown != nullptr)
            {
                m_onNodeDown(m_nodeContext, gone);
            }
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
