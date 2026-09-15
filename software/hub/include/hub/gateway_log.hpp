#pragma once

/// @file
/// @brief The log concept on the websocket side: the lines and the module
///        tables the log consumer holds, plus this node's own lines, all
///        published as NodeLogLines and NodeLogModules.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <string>

#include "gateway.pb.h"
#include "hub/gateway_publisher.hpp"
#include "log/consumer.hpp"
#include "log/sink.hpp"

namespace mark4
{
    /// Publishes every log line the gateway hears and every module table it
    /// pulled, and keeps the last lines of each node so a client that
    /// connects reads what happened before it did. It is also the local sink
    /// of this node's own provider: a messenger refuses this node as a
    /// destination, so the gateway's own lines come in here and leave as
    /// NodeLogLines from its own id, exactly like every other node's.
    class LogGateway final : public AbsLogConsumerListener, public AbsLogSink
    {
      public:
        /// Lines kept per node, and what one NodeLogLines carries at most.
        static constexpr std::size_t LOG_RING = 256U;

        /// @param publisher where the messages go
        /// @param consumer consumer this gateway listens to and reads
        /// @param ownNodeId this gateway's node id, which its own lines
        ///        carry
        LogGateway(AbsGatewayPublisher &publisher,
                   LogConsumerBase &consumer,
                   std::uint32_t ownNodeId);

        /// @brief One node's module table changed: published whole.
        /// @param nodeId the node
        /// @param modules its table, as the consumer holds it
        void onModules(std::uint32_t nodeId, std::span<const mark4_LogModuleInfo> modules) override;

        /// @brief One line of one node: kept in that node's ring and
        ///        published at once.
        /// @param nodeId node it came from
        /// @param line the line
        void onLine(std::uint32_t nodeId, const mark4_Log &line) override;

        /// @brief A node went down: its ring goes with it and its module
        ///        table is published empty, because the ids of the next boot
        ///        are its own.
        /// @param nodeId the node
        void onForgotten(std::uint32_t nodeId) override;

        /// @brief One line of this node, from its own provider.
        /// @param record the line
        void write(const LogRecord &record) override;

        /// @brief A client connected: every module table, then the ring of
        ///        every node, oldest line first, to that client alone.
        /// @param clientId the client
        void onClientConnected(const std::string &clientId);

        /// @brief Carries out one log command: a table pulled again, or one
        ///        module moved to one level. A level named on this gateway's
        ///        own id is moved in its own registry, because a node cannot
        ///        send to itself.
        /// @param command what the client asked
        /// @param clientId connection it came from, unused: a module table
        ///        is everyone's
        /// @param[out] errorOut receives the refusal reason
        /// @return true when it was carried out
        bool apply(const mark4_LogCommand &command,
                   const std::string &clientId,
                   std::string &errorOut);

      private:
        /// @brief Publishes one node's module table, as the consumer holds
        ///        it.
        /// @param nodeId the node
        /// @param modules its table
        void publishModules(std::uint32_t nodeId, std::span<const mark4_LogModuleInfo> modules);

        /// @brief Publishes this gateway's own module table, read from its
        ///        own registry rather than from the wire.
        void publishOwnModules();

        /// @brief Keeps one line in its node's ring and publishes it.
        /// @param nodeId node it came from
        /// @param line the line
        void publishLine(std::uint32_t nodeId, const mark4_Log &line);

        /// @brief Moves one module of this gateway itself.
        /// @param request module and level asked for
        /// @param[out] errorOut receives the refusal reason
        /// @return true when the module exists
        bool applyOwnLevel(const mark4_LogLevelRequest &request, std::string &errorOut);

        AbsGatewayPublisher &m_publisher; ///< where the messages go
        LogConsumerBase &m_consumer;      ///< what this gateway publishes
        std::uint32_t m_ownNodeId;        ///< this node, the source of its own lines
        /// The last LOG_RING lines of every node heard, by node id: what a
        /// client that connects late reads. Hub code, so it may allocate.
        std::map<std::uint32_t, std::deque<mark4_Log>> m_rings;
    };
} // namespace mark4
