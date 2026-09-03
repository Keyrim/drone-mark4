#include "telemetry/registry.hpp"

namespace mark4
{
    namespace
    {
        // Constant-initialized, so an entry constructed during the dynamic
        // initialization of another translation unit always finds a valid
        // list. The tail is what keeps the list in construction order: ids
        // are indexes into it, so the order has to be the same on every run
        // of the same build.
        TelemetryEntry *g_head = nullptr;
        TelemetryEntry *g_tail = nullptr;
        std::size_t g_count = 0U;
    } // namespace

    TelemetryEntry::TelemetryEntry(const char *name, TelemetryUnit unit, const float &value)
        : m_name(name),
          m_unit(unit),
          m_value(&value)
    {
        link();
    }

    TelemetryEntry::TelemetryEntry(const char *name,
                                   TelemetryUnit unit,
                                   const void *context,
                                   ReadFn read)
        : m_name(name),
          m_unit(unit),
          m_context(context),
          m_read(read)
    {
        link();
    }

    void TelemetryEntry::link()
    {
        if (g_tail == nullptr)
        {
            g_head = this;
        }
        else
        {
            g_tail->m_next = this;
        }
        g_tail = this;
        ++g_count;
    }

    TelemetryEntry::~TelemetryEntry()
    {
        // Singly linked on purpose: an entry is one pointer wider than the
        // value it measures, and unlinking happens when an object dies, not
        // in the loop. Walking to the predecessor costs nothing there.
        TelemetryEntry *previous = nullptr;
        for (TelemetryEntry *entry = g_head; entry != nullptr; entry = entry->m_next)
        {
            if (entry != this)
            {
                previous = entry;
                continue;
            }
            if (previous == nullptr)
            {
                g_head = m_next;
            }
            else
            {
                previous->m_next = m_next;
            }
            if (g_tail == this)
            {
                g_tail = previous;
            }
            --g_count;
            m_next = nullptr;
            return;
        }
    }

    TelemetryEntry *telemetryEntries()
    {
        return g_head;
    }

    std::size_t telemetryEntryCount()
    {
        return g_count;
    }
} // namespace mark4
