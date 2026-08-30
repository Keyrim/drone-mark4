#include "log/wire.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "log/module.hpp"
#include "log/module_ids.hpp"

namespace mark4
{
    namespace
    {
        LogModule MODULE{LOG_MODULE_CORE, "log/core"};

        /// Modules per LogModules page, the bound of mark4.LogModules.modules.
        constexpr std::size_t PAGE_SIZE =
            sizeof(mark4_LogModules::modules) / sizeof(mark4_LogModuleInfo);

        static_assert(sizeof(mark4_Log::text) == LogModule::MAX_TEXT + 1U,
                      "mark4.Log.text and LogModule::MAX_TEXT must agree");
        static_assert(sizeof(mark4_LogModuleInfo::name) == LogModule::MAX_NAME + 1U,
                      "mark4.LogModuleInfo.name and LogModule::MAX_NAME must agree");

        bool sendEnvelope(LogSendFn send, void *context, const mark4_Envelope &envelope)
        {
            std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
            std::size_t size = 0U;
            return encodeEnvelope(envelope, bytes.data(), bytes.size(), size) &&
                   send(context, bytes.data(), size);
        }
    } // namespace

    bool logLevelFromWire(mark4_LogLevel wire, LogLevel &levelOut)
    {
        if (wire < mark4_LogLevel_TRACE || wire > mark4_LogLevel_ERROR)
        {
            return false;
        }
        levelOut = static_cast<LogLevel>(wire);
        return true;
    }

    void TransportSink::write(const LogRecord &record)
    {
        if (record.timestampUs - m_windowStartUs >= WINDOW_US)
        {
            m_windowStartUs = record.timestampUs;
            m_windowCount = 0U;
            if (m_windowDropped > 0U)
            {
                // Said once per second, straight to the wire: a line through
                // the module would meet this same limit.
                char text[LogModule::MAX_TEXT + 1U];
                static_cast<void>(std::snprintf(text,
                                                sizeof(text),
                                                "%lu lines dropped by the rate limit",
                                                static_cast<unsigned long>(m_windowDropped)));
                LogRecord notice;
                notice.moduleId = MODULE.id();
                notice.moduleName = MODULE.name();
                notice.level = LogLevel::WARN;
                notice.timestampUs = record.timestampUs;
                notice.text = text;
                send(notice);
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
        send(record);
    }

    void TransportSink::send(const LogRecord &record)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_log_tag;
        envelope.body.log.timestamp_us = record.timestampUs;
        envelope.body.log.level = logLevelToWire(record.level);
        envelope.body.log.module_id = record.moduleId;
        std::strncpy(envelope.body.log.text, record.text, LogModule::MAX_TEXT);
        static_cast<void>(sendEnvelope(m_send, m_context, envelope));
    }

    bool logPublishModules(LogSendFn send, void *context)
    {
        const std::size_t total = logModuleCount();
        LogModule *module = logModules();
        std::size_t index = 0U;
        do
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_log_modules_tag;
            mark4_LogModules &page = envelope.body.log_modules;
            page.start_index = static_cast<std::uint32_t>(index);
            page.total = static_cast<std::uint32_t>(total);
            while (module != nullptr && page.modules_count < PAGE_SIZE)
            {
                mark4_LogModuleInfo &info = page.modules[page.modules_count];
                info.id = module->id();
                std::strncpy(info.name, module->name(), LogModule::MAX_NAME);
                info.level = logLevelToWire(module->level());
                ++page.modules_count;
                ++index;
                module = module->next();
            }
            if (!sendEnvelope(send, context, envelope))
            {
                return false;
            }
        } while (module != nullptr);
        return true;
    }

    bool logHandleControl(const mark4_LogControl &control)
    {
        switch (control.which_request)
        {
            case mark4_LogControl_query_tag:
                return true;
            case mark4_LogControl_set_tag: {
                LogLevel level = LogLevel::INFO;
                if (control.request.set.module_id > UINT16_MAX ||
                    !logLevelFromWire(control.request.set.level, level))
                {
                    return false;
                }
                LogModule *module =
                    logFindModule(static_cast<std::uint16_t>(control.request.set.module_id));
                if (module == nullptr || module->level() == level)
                {
                    return false;
                }
                module->setLevel(level);
                return true;
            }
            default:
                return false;
        }
    }
} // namespace mark4
