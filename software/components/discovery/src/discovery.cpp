#include "discovery/discovery.hpp"

#include <cstdint>
#include <span>

#include <pb.h>

#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    Discovery::Discovery(Messenger &messenger, const mark4_Announce &self)
        : Discovery(messenger, self, TAGS)
    {
    }

    Discovery::Discovery(Messenger &messenger,
                         const mark4_Announce &self,
                         std::span<const pb_size_t> tags)
        : AbsMessageHandler(messenger, tags),
          m_messenger(messenger),
          m_self(self)
    {
    }

    bool Discovery::onMessage(std::uint32_t src,
                              const mark4_Envelope &envelope,
                              std::uint64_t nowUs)
    {
        static_cast<void>(envelope); // the request carries nothing
        static_cast<void>(nowUs);
        mark4_Envelope answer = mark4_Envelope_init_zero;
        answer.which_body = mark4_Envelope_announce_tag;
        answer.body.announce = m_self;
        // An answer that matters is a request of its own: the messenger
        // keeps it until the requester acknowledges it, and gives up on its
        // own policy. Whether this one was taken is that table's business.
        static_cast<void>(request(src, answer));
        ++m_answered;
        return true;
    }
} // namespace mark4
