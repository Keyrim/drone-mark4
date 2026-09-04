#pragma once

/// @file
/// @brief C ABI of the transport for the mobile app: one node holding one
///        UDP link, driven from Dart through dart:ffi. This header is the
///        input of ffigen, so it is plain C: opaque handle, flat structs,
///        integers and byte buffers, and no callback across the boundary.
///        The C++ side accumulates what it receives, Dart polls and drains.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/// Largest payload a frame carries; a longer send is refused.
#define MARK4_MAX_PAYLOAD 512

/// Largest beacon payload accepted by mark4_transport_set_beacon().
#define MARK4_MAX_BEACON_SIZE 64

/// Payloads kept between two polls; the oldest is dropped past that.
#define MARK4_RX_QUEUE_SIZE 64

    /// One transport node: the transport, its link and the receive queue.
    typedef struct Mark4Transport Mark4Transport;

    /// One live node of the transport's table.
    typedef struct
    {
        uint32_t id;           ///< node id, never 0
        uint32_t host;         ///< IPv4 address, host byte order
        uint16_t port;         ///< UDP port
        uint64_t last_seen_us; ///< instant of the last frame from it [us]
        uint32_t received;     ///< frames accepted from it
        uint32_t lost;         ///< frames the numbering says never arrived
        uint32_t duplicates;   ///< frames carrying an already seen number
    } Mark4NodeInfo;

    /// Counters of one node, transport and shim together.
    typedef struct
    {
        uint32_t sent;          ///< frames handed to the link, beacons included
        uint64_t sent_bytes;    ///< payload bytes of those frames
        uint32_t refused;       ///< sends that reached no link
        uint32_t dropped;       ///< frames the transport dropped
        uint32_t rx_overflow;   ///< payloads the receive queue had to drop
        uint16_t data_port;     ///< port of the unicast socket
        bool loopback_fallback; ///< a broadcast had no route and used the loopback
    } Mark4TransportStats;

    /// @brief Draws a node id from /dev/urandom.
    /// @return the id, never 0; 0 when the random source cannot be read
    uint32_t mark4_random_node_id(void);

    /// @brief Creates one node with one UDP link and opens its sockets.
    /// @param node_id identity of this node, never 0
    /// @param discovery_port shared broadcast port of the deployment
    /// @return the node, NULL when the sockets could not be opened
    Mark4Transport *mark4_transport_create(uint32_t node_id, uint16_t discovery_port);

    /// @brief Closes the sockets and frees the node. NULL is accepted.
    /// @param transport node to destroy
    void mark4_transport_destroy(Mark4Transport *transport);

    /// @param transport node
    /// @return identity of the node
    uint32_t mark4_transport_node_id(const Mark4Transport *transport);

    /// @brief Registers the beacon broadcast every second and unicast to
    ///        every node the moment it appears. Copied.
    /// @param transport node
    /// @param payload beacon bytes
    /// @param size beacon size, at most MARK4_MAX_BEACON_SIZE, 0 to stop
    /// @return false when the payload is too long
    bool mark4_transport_set_beacon(Mark4Transport *transport, const uint8_t *payload, size_t size);

    /// @brief Sends one payload.
    /// @param transport node
    /// @param dst node to reach, 0 for every node
    /// @param payload payload bytes
    /// @param size payload size, at most MARK4_MAX_PAYLOAD
    /// @return true when the frame left on the link
    bool mark4_transport_send(Mark4Transport *transport,
                              uint32_t dst,
                              const uint8_t *payload,
                              size_t size);

    /// @brief Drains the link: learns nodes, queues the payloads for this
    ///        node, expires the silent nodes and emits the beacon when due.
    /// @param transport node
    /// @param now_us current instant [us], from the caller's monotonic clock
    /// @return payloads waiting in the receive queue after the poll
    size_t mark4_transport_poll(Mark4Transport *transport, uint64_t now_us);

    /// @brief Takes the oldest received payload out of the queue.
    /// @param transport node
    /// @param[out] src_out receives the sender's node id
    /// @param[out] buffer receives the payload bytes
    /// @param capacity size of buffer; a payload longer than it is dropped
    /// @return payload size, 0 when the queue is empty
    size_t mark4_transport_next_payload(Mark4Transport *transport,
                                        uint32_t *src_out,
                                        uint8_t *buffer,
                                        size_t capacity);

    /// @param transport node
    /// @return live nodes, as of the last poll
    size_t mark4_transport_node_count(const Mark4Transport *transport);

    /// @brief Reads one live node.
    /// @param transport node
    /// @param index 0 <= index < mark4_transport_node_count()
    /// @param[out] node_out receives the node
    /// @return false when the index is out of range
    bool mark4_transport_node_at(const Mark4Transport *transport,
                                 size_t index,
                                 Mark4NodeInfo *node_out);

    /// @brief Reads the counters.
    /// @param transport node
    /// @param[out] stats_out receives them
    void mark4_transport_stats(const Mark4Transport *transport, Mark4TransportStats *stats_out);

#ifdef __cplusplus
}
#endif
