/// @file
/// @brief Pilot gateway implementation.

#include "hub/gateway_pilot.hpp"

#include <map>

#include "hub/gateway_codec.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    bool PilotGateway::apply(const mark4_PilotInput &input,
                             const std::string &clientId,
                             std::uint64_t nowUs,
                             std::string &errorOut)
    {
        tick(nowUs);
        const auto seat = m_seats.find(input.node);
        if (seat != m_seats.end() && seat->second.clientId != clientId)
        {
            errorOut = "node piloted by another client";
            return false;
        }
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_rc_tag;
        envelope.body.rc = input.rc;
        if (!m_messenger.send(input.node, envelope))
        {
            errorOut = "node " + hexNodeId(input.node) + " is not reachable";
            return false;
        }
        // The seat is taken by the first client that pilots the node, and
        // fed by every input afterwards: the stream itself is the claim.
        m_seats[input.node] = Seat{clientId, nowUs};
        return true;
    }

    void PilotGateway::tick(std::uint64_t nowUs)
    {
        std::erase_if(m_seats, [nowUs](const auto &entry) {
            return nowUs - entry.second.lastUs > RC_PILOT_WINDOW_US;
        });
    }

    void PilotGateway::onClientClosed(const std::string &clientId)
    {
        std::erase_if(m_seats,
                      [&clientId](const auto &entry) { return entry.second.clientId == clientId; });
    }
} // namespace mark4
