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

#include "messaging/messenger.hpp"
#include "platform/clock.hpp"
#include "protocol/envelope.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

namespace mark4
{
    /// The messenger's handler for the SimSensor messages: it polls the
    /// messenger, which dispatches every other message to its own handler,
    /// and stashes the sensor message for the sensor source. waitSensor()
    /// is the one blocking point of the flight loop: a poll(2) on the link's
    /// sockets bounded by the caller's deadline, so the keepalive and the
    /// node expiry keep running while the plant is silent.
    ///
    /// The plant is the node the sensor source adopted, and the sensor
    /// source alone decides when there is none. Replies are cached so a
    /// resent tick gets the exact answer it missed.
    class PlantLink final : public AbsMessageHandler
    {
      public:
        /// Body tags this handler consumes.
        static constexpr std::array<pb_size_t, 1> TAGS = {mark4_Envelope_sim_sensor_tag};

        /// Largest reply the repeat cache holds: a SimActuator envelope.
        static constexpr std::size_t MAX_REPLY_SIZE = 128U;

        /// @param messenger this node's messenger, polled here; must outlive
        ///        the link
        /// @param transport this node's transport, initialized by the root;
        ///        the replies to the plant leave by it
        /// @param link its UDP link, for the descriptors waitSensor() sleeps on
        /// @param clock wall clock the messenger is polled on (the keepalive
        ///        cadence is a real-time contract whatever the sim time scale)
        PlantLink(Messenger &messenger, Transport &transport, UdpLink &link, AbsClock &clock)
            : AbsMessageHandler(messenger, TAGS),
              m_messenger(messenger),
              m_transport(transport),
              m_link(link),
              m_clock(clock)
        {
        }

        /// @brief Polls the messenger once, without blocking: every message
        ///        delivered goes to its handler, a SimSensor lands here.
        void poll();

        /// @brief Stashes one SimSensor message for the sensor source; a
        ///        message already waiting is overwritten and counted.
        /// @param src node it came from
        /// @param envelope the message
        /// @param nowUs instant of the poll [us], unused: the message carries
        ///        the plant's own time
        /// @return true, always: every sensor message is taken
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override;

        /// @brief Blocks until a SimSensor envelope arrives from any node or
        ///        the clock reaches the deadline; every other message
        ///        delivered meanwhile went to its own handler.
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

        Messenger &m_messenger;     ///< this node's messenger, polled here
        Transport &m_transport;     ///< this node's transport, for the replies
        UdpLink &m_link;            ///< its UDP link
        AbsClock &m_clock;          ///< wall clock for the messenger
        std::uint32_t m_plant = 0U; ///< adopted plant, 0 = none
        mark4_SimSensor m_pending = mark4_SimSensor_init_zero;  ///< stashed sensor message
        std::uint32_t m_pendingSrc = 0U;                        ///< node it came from
        bool m_hasPending = false;                              ///< m_pending holds a message
        std::uint32_t m_overruns = 0U;                          ///< stashed messages overwritten
        std::array<std::uint8_t, MAX_REPLY_SIZE> m_lastReply{}; ///< last reply bytes
        std::size_t m_lastReplySize = 0U;                       ///< bytes cached, 0 = none
    };
} // namespace mark4
