#include "discovery/discovery_directory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "discovery/discovery.hpp"
#include "mark4.pb.h"
#include "messaging/messenger.hpp"
#include "protocol/wire_hash.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    namespace
    {
        /// @return true when two announces say the same thing. Field by
        ///         field: the struct has padding a memcmp would read.
        bool sameAnnounce(const mark4_Announce &a, const mark4_Announce &b)
        {
            return a.kind == b.kind && a.mcu == b.mcu && a.build_epoch == b.build_epoch &&
                   a.wire_hash == b.wire_hash && std::strcmp(a.name, b.name) == 0 &&
                   std::strcmp(a.git_hash, b.git_hash) == 0;
        }
    } // namespace

    DiscoveryDirectory::DiscoveryDirectory(Messenger &messenger,
                                           Transport &transport,
                                           const mark4_Announce &self)
        : Discovery(messenger, self, TAGS),
          m_transport(transport)
    {
    }

    bool DiscoveryDirectory::onMessage(std::uint32_t src,
                                       const mark4_Envelope &envelope,
                                       std::uint64_t nowUs)
    {
        if (envelope.which_body != mark4_Envelope_announce_tag)
        {
            return Discovery::onMessage(src, envelope, nowUs);
        }
        const mark4_Announce &announce = envelope.body.announce;
        DirectoryEntry *entry = lookup(src);
        if (entry == nullptr)
        {
            // The transport learnt the node before this directory existed,
            // or the table was full when it appeared: the answer makes the
            // entry, when there is room for one.
            if (m_count >= MAX_ENTRIES)
            {
                return true;
            }
            entry = &m_entries[m_count];
            ++m_count;
            *entry = DirectoryEntry{};
            entry->id = src;
        }
        const bool changed = entry->state != DirectoryEntry::State::KNOWN ||
                             !sameAnnounce(entry->announce, announce);
        entry->announce = announce;
        entry->wireMismatch = announce.wire_hash != WIRE_HASH;
        const Transport::Node *node = m_transport.findNode(src);
        if (node != nullptr)
        {
            entry->hops = node->hops;
        }
        // A MUTE node that answers late is KNOWN like any other.
        entry->state = DirectoryEntry::State::KNOWN;
        ++m_learnt;
        if (changed)
        {
            entry->updatedUs = nowUs;
            for (std::size_t index = 0U; index < m_listenerCount; ++index)
            {
                m_listeners[index]->onIdentity(*entry);
            }
        }
        return true;
    }

    void DiscoveryDirectory::onNodeUp(std::uint32_t nodeId)
    {
        DirectoryEntry *entry = lookup(nodeId);
        if (entry == nullptr)
        {
            if (m_count >= MAX_ENTRIES)
            {
                ++m_dropped;
                return;
            }
            entry = &m_entries[m_count];
            ++m_count;
        }
        // No instant here: the entry waits for the next tick(), which asks a
        // PENDING entry with no request behind it at once. An id already
        // present (the transport says up once per learn cycle, and again for
        // a node that rebooted, which starts over the same way) is reset.
        *entry = DirectoryEntry{};
        entry->id = nodeId;
        const Transport::Node *node = m_transport.findNode(nodeId);
        if (node != nullptr)
        {
            entry->hops = node->hops;
        }
    }

    void DiscoveryDirectory::onNodeDown(std::uint32_t nodeId)
    {
        DirectoryEntry *entry = lookup(nodeId);
        if (entry == nullptr)
        {
            return;
        }
        // The order of the entries carries no meaning, so the last one takes
        // the freed slot rather than shifting the whole prefix down.
        const std::size_t last = m_count - 1U;
        *entry = m_entries[last];
        m_entries[last] = DirectoryEntry{};
        m_count = last;
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            m_listeners[index]->onForgotten(nodeId);
        }
    }

    void DiscoveryDirectory::onRequestFailed(std::uint32_t dst, std::uint32_t requestId)
    {
        static_cast<void>(requestId); // one request per entry at a time
        DirectoryEntry *entry = lookup(dst);
        if (entry == nullptr || entry->state != DirectoryEntry::State::PENDING)
        {
            // Answered in the meantime, or gone: the request that ran out of
            // sends has nothing left to say.
            return;
        }
        entry->state = DirectoryEntry::State::MUTE;
        entry->updatedUs = m_lastTickUs;
        ++m_muted;
    }

    void DiscoveryDirectory::tick(std::uint64_t nowUs)
    {
        m_lastTickUs = nowUs;
        for (std::size_t index = 0U; index < m_count; ++index)
        {
            DirectoryEntry &entry = m_entries[index];
            // Only the entries nobody has asked yet: once a request is with
            // the messenger, the resends and the giving up are its business.
            if (entry.state == DirectoryEntry::State::PENDING && entry.requests == 0U)
            {
                ask(entry, nowUs);
            }
        }
    }

    const DirectoryEntry *DiscoveryDirectory::find(std::uint32_t id) const
    {
        for (std::size_t index = 0U; index < m_count; ++index)
        {
            if (m_entries[index].id == id)
            {
                return &m_entries[index];
            }
        }
        return nullptr;
    }

    std::size_t DiscoveryDirectory::nodesOfKind(std::span<const mark4_NodeKind> kinds,
                                                std::span<DirectoryEntry> out) const
    {
        std::size_t written = 0U;
        for (std::size_t index = 0U; index < m_count && written < out.size(); ++index)
        {
            const DirectoryEntry &candidate = m_entries[index];
            if (candidate.state != DirectoryEntry::State::KNOWN)
            {
                continue;
            }
            for (const mark4_NodeKind kind : kinds)
            {
                if (candidate.announce.kind == kind)
                {
                    out[written] = candidate;
                    ++written;
                    break;
                }
            }
        }
        return written;
    }

    void DiscoveryDirectory::attach(AbsDirectoryListener &listener)
    {
        if (m_listenerCount >= MAX_LISTENERS)
        {
            ++m_listenersOverflow;
            return;
        }
        m_listeners[m_listenerCount] = &listener;
        ++m_listenerCount;
    }

    void DiscoveryDirectory::detach(AbsDirectoryListener &listener)
    {
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            if (m_listeners[index] == &listener)
            {
                // Shift the rest down: the ones that stay keep the order
                // they attached in.
                --m_listenerCount;
                for (std::size_t next = index; next < m_listenerCount; ++next)
                {
                    m_listeners[next] = m_listeners[next + 1U];
                }
                m_listeners[m_listenerCount] = nullptr;
                return;
            }
        }
        // Not in the table: it found it full when it attached, and init()
        // no longer has to report it.
        if (m_listenersOverflow > 0U)
        {
            --m_listenersOverflow;
        }
    }

    DirectoryEntry *DiscoveryDirectory::lookup(std::uint32_t id)
    {
        for (std::size_t index = 0U; index < m_count; ++index)
        {
            if (m_entries[index].id == id)
            {
                return &m_entries[index];
            }
        }
        return nullptr;
    }

    void DiscoveryDirectory::ask(DirectoryEntry &entry, std::uint64_t nowUs)
    {
        mark4_Envelope question = mark4_Envelope_init_zero;
        question.which_body = mark4_Envelope_identity_request_tag;
        const RequestPolicy policy{IDENTITY_TIMEOUT_US, IDENTITY_RETRIES};
        if (request(entry.id, question, policy) == 0U)
        {
            // The transport does not hold the node yet, or the pending table
            // is full: the entry stays untouched and the next tick asks.
            ++m_requests;
            return;
        }
        entry.askedUs = nowUs;
        entry.requests = 1U;
        ++m_requests;
    }
} // namespace mark4
