#pragma once

/// @file
/// @brief The sim link over the transport: the plant is one node of the
///        LAN, and the SimSensor / SimActuator / SimScenario envelopes of
///        the lockstep exchange are unicast frames between the two node
///        ids. Owned by the composition root, shared by the sensor source
///        (which waits here) and the motor sink (which replies here).

#include <array>
#include <cstddef>
#include <cstdint>

#include "platform/clock.hpp"
#include "platform_common/command_receiver_transport.hpp"
#include "protocol/envelope.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

namespace mark4
{
    /// Pumps the transport and sorts what it delivers: a SimSensor envelope
    /// is stashed for the sensor source, everything else goes to the command
    /// ring. waitSensor() is the one blocking point of the flight loop: a
    /// poll(2) on the link's sockets bounded by the caller's deadline, so
    /// the beacon and the node expiry keep running while the plant is
    /// silent.
    ///
    /// The plant is the node the sensor source adopted, and the sensor
    /// source alone decides when there is none. Replies are cached so a
    /// resent tick gets the exact answer it missed.
    class PlantLink
    {
      public:
        /// Largest reply the repeat cache holds: a SimActuator envelope.
        static constexpr std::size_t MAX_REPLY_SIZE = 128U;

        /// @param transport this node's transport, initialized by the root
        /// @param link its UDP link, for the descriptors waitSensor() sleeps on
        /// @param clock wall clock the transport is polled on (the beacon
        ///        cadence is a real-time contract whatever the sim time scale)
        /// @param commands ring every non-sensor payload is queued in
        PlantLink(Transport &transport,
                  UdpLink &link,
                  AbsClock &clock,
                  CommandReceiverTransport &commands)
            : m_transport(transport),
              m_link(link),
              m_clock(clock),
              m_commands(commands)
        {
        }

        /// @brief Pumps the transport once, without blocking.
        void poll();

        /// @brief Blocks until a SimSensor envelope arrives from any node or
        ///        the clock reaches the deadline; every other payload
        ///        delivered meanwhile lands in the command ring.
        /// @param[out] sensorOut the envelope body
        /// @param[out] srcOut node it came from
        /// @param deadlineUs clock instant the wait gives up at [us]
        /// @return true when a sensor message was taken
        bool waitSensor(mark4_SimSensor &sensorOut,
                        std::uint32_t &srcOut,
                        std::uint64_t deadlineUs);

        /// @return node id of the plant, 0 when none drives
        [[nodiscard]] std::uint32_t plant() const
        {
            return m_plant;
        }

        /// @brief Adopts one node as the plant (or none, with 0): the sensor
        ///        source calls it once a SimSensor validated, never on a
        ///        stray payload, and when the plant fell silent.
        /// @param nodeId the plant's node id, 0 for none
        void setPlant(std::uint32_t nodeId)
        {
            m_plant = nodeId;
            m_lastReplySize = 0U;
        }

        /// @brief Unicasts one envelope to the plant and caches it for
        ///        repeatLastReply().
        /// @param data encoded envelope
        /// @param size byte count
        /// @return true when the frame left
        bool reply(const std::uint8_t *data, std::size_t size);

        /// @brief Unicasts one envelope to the plant without touching the
        ///        repeat cache (a scenario riding next to the replies).
        /// @param data encoded envelope
        /// @param size byte count
        /// @return true when the frame left
        bool send(const std::uint8_t *data, std::size_t size);

        /// @brief Sends the last reply again, byte for byte: a plant that
        ///        resends a tick is a plant that never got its answer.
        /// @return true when a reply was cached and the frame left
        bool repeatLastReply();

        /// @return sensor messages that arrived while one was already
        ///         stashed and overwrote it (a plant running ahead of the
        ///         loop, never in lockstep)
        [[nodiscard]] std::uint32_t overruns() const
        {
            return m_overruns;
        }

      private:
        static constexpr std::uint64_t US_PER_MS = 1000U;

        /// @brief Transport delivery callback.
        /// @param context the PlantLink
        /// @param src node the payload came from
        /// @param payload payload bytes
        /// @param size payload size
        static void OnPayload(void *context,
                              std::uint32_t src,
                              const std::uint8_t *payload,
                              std::size_t size);

        Transport &m_transport;                                ///< this node's transport
        UdpLink &m_link;                                       ///< its UDP link
        AbsClock &m_clock;                                     ///< wall clock for the transport
        CommandReceiverTransport &m_commands;                  ///< everything but sensors goes here
        std::uint32_t m_plant = 0U;                            ///< adopted plant, 0 = none
        mark4_SimSensor m_pending = mark4_SimSensor_init_zero; ///< stashed sensor message
        std::uint32_t m_pendingSrc = 0U;                       ///< node it came from
        bool m_hasPending = false;                             ///< m_pending holds a message
        std::uint32_t m_overruns = 0U;                         ///< stashed messages overwritten
        std::array<std::uint8_t, MAX_REPLY_SIZE> m_lastReply{}; ///< last reply bytes
        std::size_t m_lastReplySize = 0U;                       ///< bytes cached, 0 = none
    };
} // namespace mark4
