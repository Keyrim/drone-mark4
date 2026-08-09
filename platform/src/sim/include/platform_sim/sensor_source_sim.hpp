#pragma once

/// @file
/// @brief UDP sensor source for the sim variant.

#include <cstdint>

#include "platform/sensor_source.hpp"
#include "platform_sim/udp_link.hpp"

namespace mark4
{
    /// Turns sensor packets received on the sim link into sensor frames. The
    /// simulator drives the cadence: waitFrame() blocks on the socket, and the
    /// timestamp of the frame is the simulation time carried by the packet.
    class SensorSourceSim final : public AbsSensorSource
    {
      public:
        /// @param link bound sim link the sensor packets arrive on
        explicit SensorSourceSim(UdpLink &link)
            : m_link(link)
        {
        }

        /// @brief Blocks until a valid sensor packet arrives. Datagrams of an
        ///        unexpected size or protocol version are dropped silently and
        ///        the wait resumes. The RC fields of the frame are not written
        ///        here: RC arrives out-of-band through the command receiver.
        /// @param[out] frameOut frame decoded from the packet
        /// @return FRAME when a packet was decoded, TIMEOUT when the link
        ///         stayed idle for the receive timeout of the underlying
        ///         socket; never EXHAUSTED (a simulator may always reconnect)
        FrameWait waitFrame(mark4::SensorFrame &frameOut) override;

        /// @return reset counter carried by the last decoded packet. The
        ///         simulator increments it on every world reset (teleport), a
        ///         sim-only event with no place in the sensor frame: the
        ///         composition root watches it and rebuilds the flight core.
        [[nodiscard]] std::uint8_t resetCount() const
        {
            return m_resetCount;
        }

      private:
        UdpLink &m_link;                ///< sim link, owned by the composition root
        std::uint8_t m_resetCount = 0U; ///< reset counter of the last packet
    };
} // namespace mark4
