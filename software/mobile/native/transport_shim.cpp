#include "mark4/transport_shim.h"

#include <array>
#include <cstring>
#include <new>

#include "transport/frame.hpp"
#include "transport/node_id.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

static_assert(MARK4_MAX_PAYLOAD == mark4::MAX_PAYLOAD, "the C ABI repeats the frame limit");
static_assert(MARK4_MAX_BEACON_SIZE == mark4::Transport::MAX_BEACON_SIZE,
              "the C ABI repeats the beacon limit");

namespace mark4
{
    namespace
    {
        /// One received payload, waiting for Dart to take it.
        struct QueuedPayload
        {
            std::uint32_t src = 0U;                       ///< sender
            std::size_t size = 0U;                        ///< payload size
            std::array<std::uint8_t, MAX_PAYLOAD> data{}; ///< payload bytes
        };

        /// Fixed ring of received payloads: poll() pushes, Dart pops.
        class RxQueue
        {
          public:
            /// @brief Appends one payload, dropping the oldest when full.
            /// @param src sender
            /// @param payload bytes
            /// @param size byte count, at most MAX_PAYLOAD
            void push(std::uint32_t src, const std::uint8_t *payload, std::size_t size)
            {
                if (m_count == m_slots.size())
                {
                    m_head = (m_head + 1U) % m_slots.size();
                    --m_count;
                    ++m_overflow;
                }
                QueuedPayload &slot = m_slots[(m_head + m_count) % m_slots.size()];
                slot.src = src;
                slot.size = size;
                std::memcpy(slot.data.data(), payload, size);
                ++m_count;
            }

            /// @brief Removes the oldest payload.
            /// @param[out] srcOut sender
            /// @param[out] buffer receives the bytes
            /// @param capacity size of buffer
            /// @return payload size, 0 when empty or when it does not fit
            std::size_t pop(std::uint32_t &srcOut, std::uint8_t *buffer, std::size_t capacity)
            {
                if (m_count == 0U)
                {
                    return 0U;
                }
                const QueuedPayload &slot = m_slots[m_head];
                m_head = (m_head + 1U) % m_slots.size();
                --m_count;
                if (slot.size > capacity)
                {
                    ++m_overflow;
                    return 0U;
                }
                srcOut = slot.src;
                std::memcpy(buffer, slot.data.data(), slot.size);
                return slot.size;
            }

            /// @return payloads waiting
            [[nodiscard]] std::size_t count() const
            {
                return m_count;
            }

            /// @return payloads dropped because the queue was full or the
            ///         caller's buffer too small
            [[nodiscard]] std::uint32_t overflow() const
            {
                return m_overflow;
            }

          private:
            std::array<QueuedPayload, MARK4_RX_QUEUE_SIZE> m_slots{}; ///< ring storage
            std::size_t m_head = 0U;                                  ///< oldest payload
            std::size_t m_count = 0U;                                 ///< payloads waiting
            std::uint32_t m_overflow = 0U;                            ///< payloads dropped
        };
    } // namespace
} // namespace mark4

/// The handle: one UDP link, the transport over it, the receive queue.
struct Mark4Transport
{
    /// @param nodeId identity of the node
    /// @param discoveryPort shared broadcast port
    Mark4Transport(std::uint32_t nodeId, std::uint16_t discoveryPort)
        : link(discoveryPort),
          transport(nodeId)
    {
    }

    mark4::UdpLink link;        ///< the one physical link
    mark4::Transport transport; ///< the node
    mark4::RxQueue rx;          ///< payloads received, not yet taken by Dart
};

namespace
{
    /// Transport::DeliverFn: queues every payload addressed to this node.
    void Deliver(void *context, std::uint32_t src, const std::uint8_t *payload, std::size_t size)
    {
        static_cast<mark4::RxQueue *>(context)->push(src, payload, size);
    }
} // namespace

extern "C"
{
    uint32_t mark4_random_node_id(void)
    {
        return mark4::randomNodeId();
    }

    Mark4Transport *mark4_transport_create(uint32_t node_id, uint16_t discovery_port)
    {
        if (node_id == mark4::BROADCAST_NODE)
        {
            return nullptr;
        }
        auto *handle = new (std::nothrow) Mark4Transport(node_id, discovery_port);
        if (handle == nullptr)
        {
            return nullptr;
        }
        if (!handle->link.init() || !handle->transport.addLink(handle->link) ||
            !handle->transport.init())
        {
            delete handle;
            return nullptr;
        }
        return handle;
    }

    void mark4_transport_destroy(Mark4Transport *transport)
    {
        delete transport;
    }

    uint32_t mark4_transport_node_id(const Mark4Transport *transport)
    {
        return transport->transport.nodeId();
    }

    bool mark4_transport_set_beacon(Mark4Transport *transport, const uint8_t *payload, size_t size)
    {
        if (size > MARK4_MAX_BEACON_SIZE)
        {
            return false;
        }
        transport->transport.setBeacon(payload, size);
        return true;
    }

    bool mark4_transport_send(Mark4Transport *transport,
                              uint32_t dst,
                              const uint8_t *payload,
                              size_t size)
    {
        return transport->transport.send(dst, payload, size);
    }

    size_t mark4_transport_poll(Mark4Transport *transport, uint64_t now_us)
    {
        transport->transport.poll(now_us, &Deliver, &transport->rx);
        return transport->rx.count();
    }

    size_t mark4_transport_next_payload(Mark4Transport *transport,
                                        uint32_t *src_out,
                                        uint8_t *buffer,
                                        size_t capacity)
    {
        return transport->rx.pop(*src_out, buffer, capacity);
    }

    size_t mark4_transport_node_count(const Mark4Transport *transport)
    {
        return transport->transport.nodeCount();
    }

    bool mark4_transport_node_at(const Mark4Transport *transport,
                                 size_t index,
                                 Mark4NodeInfo *node_out)
    {
        if (index >= transport->transport.nodeCount())
        {
            return false;
        }
        const mark4::Transport::Node &node = transport->transport.node(index);
        node_out->id = node.id;
        node_out->host = node.address.host;
        node_out->port = node.address.port;
        node_out->last_seen_us = node.lastSeenUs;
        node_out->received = node.received;
        node_out->lost = node.lost;
        node_out->duplicates = node.duplicates;
        return true;
    }

    void mark4_transport_stats(const Mark4Transport *transport, Mark4TransportStats *stats_out)
    {
        stats_out->sent = transport->transport.sent();
        stats_out->sent_bytes = transport->transport.sentBytes();
        stats_out->refused = transport->transport.refused();
        stats_out->dropped = transport->transport.dropped();
        stats_out->rx_overflow = transport->rx.overflow();
        stats_out->data_port = transport->link.dataPort();
        stats_out->loopback_fallback = transport->link.loopbackFallback();
    }
}
