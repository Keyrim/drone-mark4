#include "log/provider.hpp"

#include <cstdio>
#include <cstring>

#include "log/module.hpp"
#include "log/module_ids.hpp"

namespace mark4
{
    namespace
    {
        LogModule CORE{LOG_MODULE_CORE, "log/core"};
        LogModule MODULE{LOG_MODULE_LOG_PROVIDER, "log/provider"};
    } // namespace

    void LogProvider::write(const LogRecord &record)
    {
        if (record.timestampUs - m_windowStartUs >= WINDOW_US)
        {
            m_windowStartUs = record.timestampUs;
            m_windowCount = 0U;
            if (m_windowDropped > 0U)
            {
                // Said once per second, straight to the subscribers: a line
                // through the module would meet this same limit.
                char text[LogModule::MAX_TEXT + 1U];
                static_cast<void>(std::snprintf(text,
                                                sizeof(text),
                                                "%lu lines dropped by the rate limit",
                                                static_cast<unsigned long>(m_windowDropped)));
                LogRecord notice;
                notice.moduleId = CORE.id();
                notice.moduleName = CORE.name();
                notice.level = LogLevel::WARN;
                notice.timestampUs = record.timestampUs;
                notice.text = text;
                emit(notice);
                ++m_windowCount;
                m_windowDropped = 0U;
            }
        }
        if (m_windowCount >= MAX_LINES_PER_SECOND)
        {
            ++m_windowDropped;
            ++m_dropped;
            return;
        }
        ++m_windowCount;
        emit(record);
    }

    void LogProvider::emit(const LogRecord &record)
    {
        if (m_localSink != nullptr)
        {
            m_localSink->write(record);
        }
        if (m_subscribers.empty())
        {
            return;
        }
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_log_tag;
        envelope.body.log.timestamp_us = record.timestampUs;
        envelope.body.log.level = logLevelToWire(record.level);
        envelope.body.log.module_id = record.moduleId;
        std::strncpy(envelope.body.log.text, record.text, LogModule::MAX_TEXT);
        for (std::size_t index = 0U; index < m_subscribers.size(); ++index)
        {
            // Stream data: sent once, never resent. A line is only worth its
            // own instant, and the subscription is what guarantees the rest.
            if (m_messenger.send(m_subscribers.id(index), envelope))
            {
                ++m_linesSent;
            }
        }
    }

    bool LogProvider::onMessage(std::uint32_t src,
                                const mark4_Envelope &envelope,
                                std::uint64_t nowUs)
    {
        static_cast<void>(nowUs);
        switch (envelope.which_body)
        {
            case mark4_Envelope_log_subscribe_tag:
                applySubscribe(src, envelope.body.log_subscribe.enabled);
                return true;
            case mark4_Envelope_log_modules_request_tag:
                sendPage(src, envelope.body.log_modules_request.cursor);
                return true;
            case mark4_Envelope_log_set_level_tag:
                applySetLevel(src, envelope.body.log_set_level);
                return true;
            default:
                return false;
        }
    }

    void LogProvider::onNodeDown(std::uint32_t nodeId)
    {
        if (m_subscribers.remove(nodeId))
        {
            MODULE.info("%08lx gone, %zu subscriber(s) left",
                        static_cast<unsigned long>(nodeId),
                        m_subscribers.size());
        }
    }

    void LogProvider::applySubscribe(std::uint32_t src, bool enabled)
    {
        bool applied = false;
        if (enabled)
        {
            applied = m_subscribers.add(src);
            if (!applied)
            {
                MODULE.warn("no room for %08lx: %zu subscribers already",
                            static_cast<unsigned long>(src),
                            m_subscribers.size());
            }
        }
        else
        {
            static_cast<void>(m_subscribers.remove(src));
        }
        mark4_Envelope answer = mark4_Envelope_init_zero;
        answer.which_body = mark4_Envelope_log_subscribe_tag;
        answer.body.log_subscribe.enabled = applied;
        static_cast<void>(request(src, answer));
    }

    void LogProvider::sendPage(std::uint32_t src, std::uint32_t cursor)
    {
        mark4_Envelope answer = mark4_Envelope_init_zero;
        answer.which_body = mark4_Envelope_log_modules_tag;
        logFillModulesPage(cursor, answer.body.log_modules);
        static_cast<void>(request(src, answer));
    }

    void LogProvider::applySetLevel(std::uint32_t src, const mark4_LogSetLevel &command)
    {
        LogLevel level = LogLevel::INFO;
        LogModule *const module =
            command.module_id > UINT16_MAX
                ? nullptr
                : logFindModule(static_cast<std::uint16_t>(command.module_id));
        if (module == nullptr)
        {
            // Nothing to move and nothing to describe: the requester asked
            // about a module this build does not have.
            MODULE.warn("%08lx asked for module %lu, which this node has none of",
                        static_cast<unsigned long>(src),
                        static_cast<unsigned long>(command.module_id));
            return;
        }
        // A level this build does not know leaves the module where it is;
        // the answer then says what it still is, which is the answer either
        // way.
        const bool known = logLevelFromWire(command.level, level);
        const bool moved = known && module->level() != level;
        if (moved)
        {
            module->setLevel(level);
        }
        sendModuleInfo(src, *module);
        if (!moved)
        {
            return;
        }
        // Subscribing to the stream means hearing what changes it: every
        // other subscriber learns the new level without asking.
        for (std::size_t index = 0U; index < m_subscribers.size(); ++index)
        {
            if (m_subscribers.id(index) != src)
            {
                sendModuleInfo(m_subscribers.id(index), *module);
            }
        }
    }

    void LogProvider::sendModuleInfo(std::uint32_t dst, const LogModule &module)
    {
        mark4_Envelope answer = mark4_Envelope_init_zero;
        answer.which_body = mark4_Envelope_log_module_info_tag;
        logFillModuleInfo(module, answer.body.log_module_info);
        static_cast<void>(request(dst, answer));
    }
} // namespace mark4
