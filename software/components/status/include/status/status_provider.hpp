#pragma once

/// @file
/// @brief The Status stream on the wire: the subscribe request in, one
///        decimated report out to every node that asked for it.

#include <array>
#include <cstddef>
#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "messaging/messenger.hpp"
#include "messaging/subscriber_table.hpp"
#include "protocol/envelope.hpp"
#include "status/status_packer.hpp"

namespace mark4
{
    /// Packs one Status report every STATUS_PERIOD_FRAMES frames and sends
    /// it to every subscriber. Owns the frame counter, so every composition
    /// decimates the same stream the same way instead of each keeping a
    /// drifting copy of the counter and the factor. With no subscriber
    /// nothing is packed at all: the report costs the loop nothing until a
    /// consumer asks for it.
    class StatusProvider final : public AbsMessageHandler
    {
      public:
        /// Body tags this handler consumes.
        static constexpr std::array<pb_size_t, 1> TAGS = {mark4_Envelope_status_subscribe_tag};

        /// Frames per Status message: a 500 Hz loop reports at 50 Hz.
        static constexpr std::uint32_t STATUS_PERIOD_FRAMES = 10U;

        /// Nodes that may hold the stream at once. Four: a board's UART pays
        /// one emission per entry, at 50 Hz, next to the log lines and the
        /// telemetry sharing the line.
        static constexpr std::size_t MAX_SUBSCRIBERS = 4U;

        /// @param messenger messenger the subscribes come from and the
        ///        reports leave by; must outlive the provider
        explicit StatusProvider(Messenger &messenger)
            : AbsMessageHandler(messenger, TAGS),
              m_messenger(messenger)
        {
        }

        /// @brief Counts one frame and sends every STATUS_PERIOD_FRAMES-th
        ///        report to every subscriber.
        /// @param frame sensor frame of this step
        /// @param actuators actuator outputs of this step
        /// @param core flight core the estimates are read from
        /// @param rcLinkOk true when the RC fail-safe is not active for this
        ///        frame, so a pilot's device can tell it is being heard
        /// @param truth the plant's exact state at this frame, nullptr when
        ///        the composition has none (a real board)
        void publish(const SensorFrame &frame,
                     const ActuatorFrame &actuators,
                     const FlightCore &core,
                     bool rcLinkOk,
                     const mark4_PlantTruth *truth = nullptr)
        {
            ++m_frameCount;
            if (m_frameCount % STATUS_PERIOD_FRAMES != 0U || m_subscribers.empty())
            {
                return;
            }
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_status_tag;
            packStatus(frame, actuators, core, rcLinkOk, envelope.body.status);
            if (truth != nullptr)
            {
                envelope.body.status.has_truth = true;
                envelope.body.status.truth = *truth;
            }
            for (std::size_t index = 0U; index < m_subscribers.size(); ++index)
            {
                // Stream data: sent once, never resent. A report is only
                // worth its own instant, and the next one is 10 frames away.
                if (m_messenger.send(m_subscribers.id(index), envelope))
                {
                    ++m_messageCount;
                }
            }
        }

        /// @brief Takes one subscribe request and answers it with what was
        ///        applied.
        /// @param src node it came from: the subscriber, and where the
        ///        answer goes
        /// @param envelope decoded message
        /// @param nowUs instant of the poll that delivered it [us], unused:
        ///        the stream is paced by the frames
        /// @return true when the message was answered
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            static_cast<void>(nowUs);
            if (envelope.which_body != mark4_Envelope_status_subscribe_tag)
            {
                return false;
            }
            bool applied = false;
            if (envelope.body.status_subscribe.enabled)
            {
                applied = m_subscribers.add(src);
                if (!applied)
                {
                    Module().warn("no room for %08lx: %zu subscribers already",
                                  static_cast<unsigned long>(src),
                                  m_subscribers.size());
                }
            }
            else
            {
                static_cast<void>(m_subscribers.remove(src));
            }
            mark4_Envelope answer = mark4_Envelope_init_zero;
            answer.which_body = mark4_Envelope_status_subscribe_tag;
            answer.body.status_subscribe.enabled = applied;
            if (request(src, answer))
            {
                ++m_answerCount;
            }
            return true;
        }

        /// @brief A node went down: it is not listening any more.
        /// @param nodeId the node
        void onNodeDown(std::uint32_t nodeId) override
        {
            if (m_subscribers.remove(nodeId))
            {
                Module().info("%08lx gone, %zu subscriber(s) left",
                              static_cast<unsigned long>(nodeId),
                              m_subscribers.size());
            }
        }

        /// @return nodes holding the stream
        [[nodiscard]] std::size_t subscribers() const
        {
            return m_subscribers.size();
        }

        /// @return Status messages sent since construction
        [[nodiscard]] std::uint32_t messagesSent() const
        {
            return m_messageCount;
        }

        /// @return answers sent to subscribe requests since construction
        [[nodiscard]] std::uint32_t answers() const
        {
            return m_answerCount;
        }

      private:
        /// @return the logging module of the provider. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_STATUS_PROVIDER, "status/provider"};
            return MODULE;
        }

        Messenger &m_messenger;                         ///< reports leave by it, not owned
        SubscriberTable<MAX_SUBSCRIBERS> m_subscribers; ///< nodes holding the stream
        std::uint32_t m_frameCount = 0U;                ///< frames seen since construction
        std::uint32_t m_messageCount = 0U;              ///< Status messages sent
        std::uint32_t m_answerCount = 0U;               ///< subscribe answers sent
    };
} // namespace mark4
