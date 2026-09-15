#pragma once

/// @file
/// @brief The log of a node on the wire: the line stream to whoever
///        subscribed, the module table one page per request, and the level
///        of one module moved from the wire.

#include <array>
#include <cstddef>
#include <cstdint>

#include "log/sink.hpp"
#include "log/wire.hpp"
#include "messaging/messenger.hpp"
#include "messaging/subscriber_table.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// The log library's one route to the wire. It is a sink of the library
    /// (every line let through leaves as a `Log` message) and a handler of
    /// the messenger (the subscribe, the table pages and the level moves),
    /// which is what makes the levels of a node and the lines it emits one
    /// conversation with the consumers that asked for them.
    ///
    /// A messenger cannot send to its own node, so a node that consumes its
    /// own lines (a gateway mirroring them to its clients) registers a local
    /// sink instead; it is fed after the rate limit, exactly like a
    /// subscriber.
    class LogProvider final : public AbsMessageHandler, public AbsLogSink
    {
      public:
        /// Body tags this handler consumes.
        static constexpr std::array<pb_size_t, 3> TAGS = {mark4_Envelope_log_subscribe_tag,
                                                          mark4_Envelope_log_modules_request_tag,
                                                          mark4_Envelope_log_set_level_tag};

        /// Nodes that may hold the line stream at once. Two: every line is
        /// one emission per entry, on a link that also carries the flight
        /// traffic.
        static constexpr std::size_t MAX_SUBSCRIBERS = 2U;

        /// Lines let through per second; the rest are dropped and counted.
        static constexpr std::uint32_t MAX_LINES_PER_SECOND = 50U;

        /// Window of the rate limit [us].
        static constexpr std::uint64_t WINDOW_US = 1'000'000U;

        /// @param messenger messenger the requests come from and the lines
        ///        and answers leave by; must outlive the provider
        explicit LogProvider(Messenger &messenger)
            : AbsMessageHandler(messenger, TAGS),
              m_messenger(messenger)
        {
        }

        /// @brief Registers the sink of this node's own lines, or removes it
        ///        with nullptr. For a node that consumes what it logs:
        ///        a messenger refuses its own node as a destination, so the
        ///        stream cannot carry it there.
        /// @param sink sink fed after the rate limit, not owned
        void setLocalSink(AbsLogSink *sink)
        {
            m_localSink = sink;
        }

        /// @brief Sends the record to every subscriber and to the local
        ///        sink. Past the rate limit the line is counted instead; the
        ///        count goes out once per second as a WARN of log/core,
        ///        directly, so it never rate limits itself.
        /// @param record the line
        void write(const LogRecord &record) override;

        /// @brief Takes one log request and answers the node that sent it.
        /// @param src node it came from: where the answer goes
        /// @param envelope decoded message
        /// @param nowUs instant of the poll that delivered it [us], unused:
        ///        the rate limit is timed on the records themselves
        /// @return true when the message was answered
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override;

        /// @brief A node went down: it is not listening any more.
        /// @param nodeId the node
        void onNodeDown(std::uint32_t nodeId) override;

        /// @return lines dropped by the rate limit, cumulative
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

        /// @return nodes holding the line stream
        [[nodiscard]] std::size_t subscribers() const
        {
            return m_subscribers.size();
        }

        /// @return Log messages sent since construction, one per line per
        ///         subscriber
        [[nodiscard]] std::uint32_t linesSent() const
        {
            return m_linesSent;
        }

      private:
        /// @brief Sends one record to every subscriber and to the local
        ///        sink, without passing the rate limit again.
        /// @param record the line
        void emit(const LogRecord &record);

        /// @brief Answers one subscribe request with the subscription as it
        ///        stands.
        /// @param src node that asked
        /// @param enabled what it asked for
        void applySubscribe(std::uint32_t src, bool enabled);

        /// @brief Answers one page request with the page it names.
        /// @param src node that asked
        /// @param cursor first module to describe
        void sendPage(std::uint32_t src, std::uint32_t cursor);

        /// @brief Moves one module's level and tells the node that asked,
        ///        then the other subscribers when the level really moved.
        /// @param src node that asked
        /// @param command the request
        void applySetLevel(std::uint32_t src, const mark4_LogSetLevel &command);

        /// @brief Sends one module's description as a request of this
        ///        provider's.
        /// @param dst node it goes to
        /// @param module module to describe
        void sendModuleInfo(std::uint32_t dst, const LogModule &module);

        Messenger &m_messenger; ///< lines and answers leave by it, not owned
        SubscriberTable<MAX_SUBSCRIBERS> m_subscribers; ///< nodes holding the line stream
        AbsLogSink *m_localSink = nullptr;              ///< this node's own consumer, not owned
        std::uint64_t m_windowStartUs = 0U;             ///< start of the current second
        std::uint32_t m_windowCount = 0U;               ///< lines let through in it
        std::uint32_t m_windowDropped = 0U;             ///< lines refused in it
        std::uint32_t m_dropped = 0U;                   ///< lines refused, cumulative
        std::uint32_t m_linesSent = 0U;                 ///< Log messages sent
    };
} // namespace mark4
