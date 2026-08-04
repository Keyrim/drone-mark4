#pragma once

/// @file
/// @brief UDP sensor source for the sim variant.

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
        ///        the wait resumes.
        /// @param[out] frameOut frame decoded from the packet
        /// @return true when a frame was decoded, false when the link stayed
        ///         idle for the receive timeout
        bool waitFrame(mark4::SensorFrame &frameOut) override;

      private:
        UdpLink &m_link; ///< sim link, owned by the composition root
    };
} // namespace mark4
