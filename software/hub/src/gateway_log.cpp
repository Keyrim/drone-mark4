/// @file
/// @brief Log gateway implementation.

#include "hub/gateway_log.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "hub/gateway_codec.hpp"
#include "log/module.hpp"
#include "log/wire.hpp"

namespace mark4
{
    namespace
    {
        /// @brief The message that publishes one node's module table.
        /// @param nodeId node the table belongs to
        /// @param modules the table
        /// @return the message
        mark4_GatewayMessage modulesMessage(std::uint32_t nodeId,
                                            std::span<const mark4_LogModuleInfo> modules)
        {
            mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
            message.which_body = mark4_GatewayMessage_node_log_modules_tag;
            fillNodeLogModules(nodeId, modules, message.body.node_log_modules);
            return message;
        }
    } // namespace

    LogGateway::LogGateway(AbsGatewayPublisher &publisher,
                           LogConsumerBase &consumer,
                           std::uint32_t ownNodeId)
        : AbsLogConsumerListener(consumer),
          m_publisher(publisher),
          m_consumer(consumer),
          m_ownNodeId(ownNodeId)
    {
    }

    void LogGateway::publishModules(std::uint32_t nodeId,
                                    std::span<const mark4_LogModuleInfo> modules)
    {
        m_publisher.broadcast(modulesMessage(nodeId, modules));
    }

    void LogGateway::publishOwnModules()
    {
        std::array<mark4_LogModuleInfo, NODE_LOG_MODULES> own{};
        const std::size_t count = ownLogModules(own);
        publishModules(m_ownNodeId, std::span<const mark4_LogModuleInfo>(own.data(), count));
    }

    void LogGateway::publishLine(std::uint32_t nodeId, const mark4_Log &line)
    {
        std::deque<mark4_Log> &ring = m_rings[nodeId];
        ring.push_back(line);
        while (ring.size() > LOG_RING)
        {
            ring.pop_front();
        }
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_node_log_lines_tag;
        message.body.node_log_lines.node = nodeId;
        message.body.node_log_lines.lines[0] = line;
        message.body.node_log_lines.lines_count = 1U;
        m_publisher.broadcast(message);
    }

    void LogGateway::onModules(std::uint32_t nodeId, std::span<const mark4_LogModuleInfo> modules)
    {
        publishModules(nodeId, modules);
    }

    void LogGateway::onLine(std::uint32_t nodeId, const mark4_Log &line)
    {
        publishLine(nodeId, line);
    }

    void LogGateway::onForgotten(std::uint32_t nodeId)
    {
        m_rings.erase(nodeId);
        publishModules(nodeId, {});
    }

    void LogGateway::write(const LogRecord &record)
    {
        mark4_Log line = mark4_Log_init_zero;
        line.timestamp_us = record.timestampUs;
        line.level = logLevelToWire(record.level);
        line.module_id = record.moduleId;
        copyWireString(record.text, line.text, sizeof(line.text));
        publishLine(m_ownNodeId, line);
    }

    void LogGateway::onClientConnected(const std::string &clientId)
    {
        publishOwnModules();
        for (std::size_t index = 0U; index < m_consumer.size(); ++index)
        {
            const LogConsumerBase::Entry &entry = m_consumer.entry(index);
            m_publisher.sendTo(clientId, modulesMessage(entry.id, entry.modules.items()));
        }
        // The ring of each node goes as one message, oldest line first: a
        // client that connects late reads what happened before it did.
        for (const auto &[nodeId, ring] : m_rings)
        {
            mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
            message.which_body = mark4_GatewayMessage_node_log_lines_tag;
            mark4_NodeLogLines &published = message.body.node_log_lines;
            published.node = nodeId;
            for (const mark4_Log &line : ring)
            {
                published.lines[published.lines_count] = line;
                ++published.lines_count;
            }
            m_publisher.sendTo(clientId, message);
        }
    }

    bool LogGateway::applyOwnLevel(const mark4_LogLevelRequest &request, std::string &errorOut)
    {
        LogModule *const module =
            request.module_id > UINT16_MAX
                ? nullptr
                : logFindModule(static_cast<std::uint16_t>(request.module_id));
        if (module == nullptr)
        {
            errorOut = "this gateway has no module " + std::to_string(request.module_id);
            return false;
        }
        LogLevel level = LogLevel::INFO;
        if (!logLevelFromWire(request.level, level))
        {
            errorOut = "unknown log level";
            return false;
        }
        module->setLevel(level);
        publishOwnModules();
        return true;
    }

    bool LogGateway::apply(const mark4_LogCommand &command,
                           const std::string &clientId,
                           std::string &errorOut)
    {
        static_cast<void>(clientId);
        switch (command.which_action)
        {
            case mark4_LogCommand_refresh_tag:
                if (!m_consumer.refresh(command.node))
                {
                    errorOut = "node " + hexNodeId(command.node) + " has no log table here";
                    return false;
                }
                return true;
            case mark4_LogCommand_set_level_tag: {
                if (command.node == m_ownNodeId)
                {
                    return applyOwnLevel(command.action.set_level, errorOut);
                }
                if (!m_consumer.setLevel(command.node,
                                         command.action.set_level.module_id,
                                         command.action.set_level.level))
                {
                    errorOut = "node " + hexNodeId(command.node) + " took no level change";
                    return false;
                }
                return true;
            }
            default:
                errorOut = "empty log command";
                return false;
        }
    }
} // namespace mark4
