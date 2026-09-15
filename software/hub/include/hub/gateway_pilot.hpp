#pragma once

/// @file
/// @brief Piloting through the gateway: one Rc forwarded per PilotInput,
///        from the gateway's own id, and one pilot per node at a time.

#include <cstdint>
#include <map>
#include <string>

#include "gateway.pb.h"
#include "messaging/messenger.hpp"

namespace mark4
{
    /// The gateway is the pilot node: it forwards each PilotInput to the
    /// node it names, once, at the cadence the client sends. Two rules the
    /// fail-safe depends on: it never repeats (a client that stops sending
    /// stops the stream, and the node's own RC timeout does the rest), and
    /// one client holds a node at a time. There is no consumer behind Rc,
    /// which is a stream and not a state, so this gateway holds the
    /// messenger itself and is no message handler.
    class PilotGateway
    {
      public:
        /// A pilot that has sent nothing for this long releases the node to
        /// whoever asks next (the stream runs at 20 Hz while engaged).
        static constexpr std::uint64_t RC_PILOT_WINDOW_US = 2'000'000U;

        /// @param messenger messenger the Rc leaves by
        explicit PilotGateway(Messenger &messenger)
            : m_messenger(messenger)
        {
        }

        /// @brief Forwards one pilot state to the node it names.
        /// @param input what the client sent
        /// @param clientId connection it came from: the seat holder
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the refusal reason
        /// @return true when the Rc went out
        bool apply(const mark4_PilotInput &input,
                   const std::string &clientId,
                   std::uint64_t nowUs,
                   std::string &errorOut);

        /// @brief Releases the seats nobody has fed for RC_PILOT_WINDOW_US.
        /// @param nowUs current time [us]
        void tick(std::uint64_t nowUs);

        /// @brief A client went away: it pilots nothing any more.
        /// @param clientId the client
        void onClientClosed(const std::string &clientId);

        /// @return seats held, what GatewayStatus.rc_clients counts
        [[nodiscard]] std::size_t seats() const
        {
            return m_seats.size();
        }

      private:
        /// Who pilots one node, and when it last said so.
        struct Seat
        {
            std::string clientId;     ///< client holding the node
            std::uint64_t lastUs = 0; ///< instant of its last input [us]
        };

        Messenger &m_messenger;                ///< where the Rc goes out
        std::map<std::uint32_t, Seat> m_seats; ///< the seat of each node piloted
    };
} // namespace mark4
