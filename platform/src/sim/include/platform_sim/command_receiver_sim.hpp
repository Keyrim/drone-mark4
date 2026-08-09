#pragma once

/// @file
/// @brief Command uplink over a UDP link, for the sim variant.

#include <cstddef>
#include <cstdint>

#include "platform/command_receiver.hpp"
#include "platform_sim/udp_link.hpp"

namespace mark4
{
    /// Hands out the protocol/ packets pending on a bound UDP link, one per
    /// poll(). poll() never blocks: the flight loop is paced by the sensor
    /// source, and the command link must never stall it. Decoding belongs
    /// to the consumer, per the interface.
    class CommandReceiverSim final : public AbsCommandReceiver
    {
      public:
        /// @param link bound link the command packets arrive on, owned by
        ///        the composition root
        explicit CommandReceiverSim(UdpLink &link)
            : m_link(link)
        {
        }

        /// @brief Takes one pending datagram, if any, without blocking.
        /// @param[out] bufferOut destination, valid only when returning > 0
        /// @param capacity size of the destination buffer in bytes
        /// @return datagram size in bytes, or 0 when nothing is pending
        std::size_t poll(std::uint8_t *bufferOut, std::size_t capacity) override;

        /// @return datagrams handed out since construction
        [[nodiscard]] std::uint32_t packetsReceived() const
        {
            return m_packetsReceived;
        }

      private:
        UdpLink &m_link;                      ///< command link, not owned
        std::uint32_t m_packetsReceived = 0U; ///< datagrams handed to the caller
    };
} // namespace mark4
