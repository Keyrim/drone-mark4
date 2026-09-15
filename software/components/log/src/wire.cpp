#include "log/wire.hpp"

#include <cstring>

#include "log/module.hpp"

namespace mark4
{
    namespace
    {
        static_assert(sizeof(mark4_Log::text) == LogModule::MAX_TEXT + 1U,
                      "mark4.Log.text and LogModule::MAX_TEXT must agree");
        static_assert(sizeof(mark4_LogModuleInfo::name) == LogModule::MAX_NAME + 1U,
                      "mark4.LogModuleInfo.name and LogModule::MAX_NAME must agree");
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

    void logFillModuleInfo(const LogModule &module, mark4_LogModuleInfo &infoOut)
    {
        infoOut = mark4_LogModuleInfo_init_zero;
        infoOut.id = module.id();
        std::strncpy(infoOut.name, module.name(), LogModule::MAX_NAME);
        infoOut.level = logLevelToWire(module.level());
    }

    void logFillModulesPage(std::uint32_t cursor, mark4_LogModules &pageOut)
    {
        pageOut = mark4_LogModules_init_zero;
        pageOut.total = static_cast<std::uint32_t>(logModuleCount());
        pageOut.cursor = cursor;
        // The registry is a list, so the cursor is walked rather than
        // indexed; a cursor past the end simply leaves the page empty.
        LogModule *module = logModules();
        for (std::uint32_t skipped = 0U; skipped < cursor && module != nullptr; ++skipped)
        {
            module = module->next();
        }
        while (module != nullptr && pageOut.modules_count < LOG_MODULES_PER_PAGE)
        {
            logFillModuleInfo(*module, pageOut.modules[pageOut.modules_count]);
            ++pageOut.modules_count;
            module = module->next();
        }
    }
} // namespace mark4
