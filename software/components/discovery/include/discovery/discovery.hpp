#pragma once

/// @file
/// @brief Who this node is, on request: the IdentityRequest of
///        `protocol/mark4.proto` in, this node's Announce out, unicast to
///        the node that asked.

#include <array>
#include <cstdint>
#include <span>

#include <pb.h>

#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Answers "who are you": the node's identity, unicast to whoever asks.
    /// Every node carries one; a node that cannot say who it is does not
    /// exist for the ground tools.
    class Discovery : public AbsMessageHandler
    {
      public:
        /// Body tags this handler consumes: the one question it answers.
        static constexpr std::array<pb_size_t, 1> TAGS = {mark4_Envelope_identity_request_tag};

        /// @param messenger messenger the requests come from and the answers
        ///        leave by; must outlive the handler
        /// @param self this node's identity, copied
        Discovery(Messenger &messenger, const mark4_Announce &self);

        /// @brief Answers one IdentityRequest with the node's Announce.
        /// @param src node that asked: where the answer goes
        /// @param envelope the request
        /// @param nowUs instant of the poll that delivered it [us], unused
        /// @return true: a request is always answered
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override;

        /// @return this node's identity, as it is announced
        [[nodiscard]] const mark4_Announce &self() const
        {
            return m_self;
        }

        /// @return requests answered since construction
        [[nodiscard]] std::uint32_t answered() const
        {
            return m_answered;
        }

      protected:
        /// @brief For a derived class that consumes more tags than the
        ///        request: the tag span must outlive the handler, like the
        ///        base handler's.
        /// @param messenger messenger to attach to
        /// @param self this node's identity, copied
        /// @param tags every body tag the derived class consumes, the
        ///        IdentityRequest included
        Discovery(Messenger &messenger,
                  const mark4_Announce &self,
                  std::span<const pb_size_t> tags);

        /// @return the messenger, for a derived class that sends too
        Messenger &accessMessenger()
        {
            return m_messenger;
        }

      private:
        Messenger &m_messenger;        ///< answer route, not owned
        mark4_Announce m_self;         ///< what this node answers
        std::uint32_t m_answered = 0U; ///< requests answered
    };
} // namespace mark4
