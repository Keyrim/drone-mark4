#pragma once

/// @file
/// @brief Who is around, by kind: a Discovery that also asks every node the
///        transport hears who it is, with a timeout and retries, and keeps
///        a directory of the answers for the nodes that need to know (a
///        gateway, a phone, a plant, a campaign).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <pb.h>

#include "discovery/discovery.hpp"
#include "mark4.pb.h"
#include "messaging/messenger.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    class DiscoveryDirectory;

    /// One node the directory knows of.
    struct DirectoryEntry
    {
        /// Where the directory stands with the node.
        enum class State : std::uint8_t
        {
            PENDING, ///< asked, no answer yet
            KNOWN,   ///< announce valid
            MUTE     ///< gave up asking
        };

        std::uint32_t id = 0U;                              ///< node id
        State state = State::PENDING;                       ///< where the directory stands
        mark4_Announce announce = mark4_Announce_init_zero; ///< valid when KNOWN
        bool wireMismatch = false;    ///< KNOWN and announce.wire_hash != WIRE_HASH
        std::uint8_t hops = 0U;       ///< distance in relays, from the transport's node
        std::uint64_t askedUs = 0U;   ///< instant of the last request [us]
        std::uint8_t requests = 0U;   ///< requests sent so far
        std::uint64_t updatedUs = 0U; ///< instant of the last state change [us]
    };

    /// Told when an identity is learnt or a node is forgotten. Attaches to
    /// the directory in its constructor and detaches in its destructor:
    /// declaring one as a member after the directory is the whole wiring
    /// (same shape as AbsPresenceListener). A listener may send() from inside
    /// its callbacks. Both bodies are inline like every other abstract class
    /// of the components: the library is built without RTTI and an
    /// out-of-line destructor would leave the typeinfo the RTTI-enabled
    /// executables reference undefined.
    class AbsDirectoryListener
    {
      public:
        /// @param directory directory to listen to; must outlive the listener
        explicit AbsDirectoryListener(DiscoveryDirectory &directory);

        virtual ~AbsDirectoryListener();

        AbsDirectoryListener(const AbsDirectoryListener &) = delete;
        AbsDirectoryListener &operator=(const AbsDirectoryListener &) = delete;

        /// @brief An entry just became KNOWN, or its announce changed.
        /// @param entry the entry, as the directory holds it
        virtual void onIdentity(const DirectoryEntry &entry) = 0;

        /// @brief The transport expired the node and its entry is gone.
        /// @param nodeId the node
        virtual void onForgotten(std::uint32_t nodeId) = 0;

      private:
        DiscoveryDirectory &m_directory; ///< where this listener is attached
    };

    /// A Discovery that also asks: who is around, by kind. For the nodes that
    /// need to know (a gateway, a phone, a plant, a campaign). It never reads
    /// a clock: the instants come from the messenger's poll and from tick().
    class DiscoveryDirectory : public Discovery, public AbsPresenceListener
    {
      public:
        /// Body tags this handler consumes: the question and the answer.
        static constexpr std::array<pb_size_t, 2> TAGS = {mark4_Envelope_identity_request_tag,
                                                          mark4_Envelope_announce_tag};

        /// Silence after a request before it is sent again [us].
        static constexpr std::uint64_t IDENTITY_TIMEOUT_US = 500'000U;

        /// Requests sent before giving up on a node.
        static constexpr std::uint8_t IDENTITY_RETRIES = 5U;

        /// Entries kept at once: one per node the transport can hold.
        static constexpr std::size_t MAX_ENTRIES = Transport::MAX_NODES;

        /// Listeners one directory may hold; init() fails past that.
        static constexpr std::size_t MAX_LISTENERS = 4U;

        /// @param messenger messenger the messages come from and leave by;
        ///        must outlive the directory
        /// @param transport transport whose presence events and node table
        ///        are read; must outlive the directory
        /// @param self this node's identity, copied, answered like any
        ///        Discovery does
        DiscoveryDirectory(Messenger &messenger, Transport &transport, const mark4_Announce &self);

        /// @brief Stores an Announce as the identity of the node it came
        ///        from; anything else is the base handler's.
        /// @param src node the message came from
        /// @param envelope the message
        /// @param nowUs instant of the poll that delivered it [us]
        /// @return true when the message was acted on
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override;

        /// @brief A node appeared: a PENDING entry the next tick() asks.
        /// @param node the node, as the transport holds it
        void onNodeUp(const Transport::Node &node) override;

        /// @brief A node expired: its entry goes, the listeners are told.
        /// @param node the node, as it was
        void onNodeDown(const Transport::Node &node) override;

        /// @brief Asks, retries and gives up on time. Call it from the
        ///        composition's loop, after the messenger's poll.
        /// @param nowUs current instant [us], from the caller's clock
        void tick(std::uint64_t nowUs);

        /// @param id node to look up
        /// @return its entry, nullptr when the directory holds none
        [[nodiscard]] const DirectoryEntry *find(std::uint32_t id) const;

        /// @return entries held
        [[nodiscard]] std::size_t size() const
        {
            return m_count;
        }

        /// @param index 0 <= index < size()
        /// @return one entry; the entries form a dense prefix whose order
        ///         carries no meaning, like the transport's node table
        [[nodiscard]] const DirectoryEntry &entry(std::size_t index) const
        {
            return m_entries[index];
        }

        /// @brief Copies the KNOWN entries whose kind is one of kinds into out.
        /// @param kinds kinds wanted
        /// @param out receives the entries, in directory order
        /// @return entries written, at most out.size()
        [[nodiscard]] std::size_t nodesOfKind(std::span<const mark4_NodeKind> kinds,
                                              std::span<DirectoryEntry> out) const;

        /// @brief Checks the composition: at most MAX_LISTENERS listeners.
        /// @return true when every listener that attached is heard
        [[nodiscard]] bool init() const
        {
            return m_listenersOverflow == 0U;
        }

        /// @return IdentityRequests sent, the refused ones included
        [[nodiscard]] std::uint32_t requests() const
        {
            return m_requests;
        }

        /// @return Announces stored as an identity
        [[nodiscard]] std::uint32_t learnt() const
        {
            return m_learnt;
        }

        /// @return nodes given up on after IDENTITY_RETRIES requests
        [[nodiscard]] std::uint32_t muted() const
        {
            return m_muted;
        }

        /// @return nodes that appeared while the table was full
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

      private:
        friend class AbsDirectoryListener;

        /// @brief Adds one listener, told of every identity learnt and node
        ///        forgotten from now on. Called by the listener's constructor.
        ///        Past MAX_LISTENERS the listener is not added and init()
        ///        fails for as long as it lives.
        /// @param listener listener to add
        void attach(AbsDirectoryListener &listener);

        /// @brief Removes one listener, if present. Called by the listener's
        ///        destructor.
        /// @param listener listener to remove
        void detach(AbsDirectoryListener &listener);

        /// @param id node to look up
        /// @return mutable entry, nullptr when unknown
        DirectoryEntry *lookup(std::uint32_t id);

        /// @brief Sends one IdentityRequest to the entry's node and counts it,
        ///        whether or not the frame left: the transport may not know
        ///        the node yet on the very first tick, the retry covers it.
        /// @param entry entry to ask
        /// @param nowUs current instant [us]
        void ask(DirectoryEntry &entry, std::uint64_t nowUs);

        Transport &m_transport;                              ///< presence and hops, not owned
        std::array<DirectoryEntry, MAX_ENTRIES> m_entries{}; ///< entries, dense prefix
        std::size_t m_count = 0U;                            ///< entries in m_entries
        std::array<AbsDirectoryListener *, MAX_LISTENERS> m_listeners{}; ///< attached, in order
        std::size_t m_listenerCount = 0U;                                ///< listeners attached
        std::uint32_t m_listenersOverflow = 0U; ///< live listeners that found the table full
        std::uint32_t m_requests = 0U;          ///< requests sent
        std::uint32_t m_learnt = 0U;            ///< announces stored
        std::uint32_t m_muted = 0U;             ///< nodes given up on
        std::uint32_t m_dropped = 0U;           ///< nodes that found the table full
    };

    inline AbsDirectoryListener::AbsDirectoryListener(DiscoveryDirectory &directory)
        : m_directory(directory)
    {
        m_directory.attach(*this);
    }

    inline AbsDirectoryListener::~AbsDirectoryListener()
    {
        m_directory.detach(*this);
    }
} // namespace mark4
