#pragma once

/// @file
/// @brief One physical link the transport emits on and receives from. A
///        link moves whole frames and knows nothing of their content; its
///        addresses are opaque to everyone but itself.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// Where a frame came from or goes to, on one link. A UDP link fills
    /// both fields (IPv4 host and port, host byte order); a point-to-point
    /// link such as a UART has nothing to say and leaves them at zero.
    struct LinkAddress
    {
        std::uint32_t host = 0U; ///< IPv4 address, host byte order, 0 = none
        std::uint16_t port = 0U; ///< UDP port, 0 = none
    };

    /// Frame mover over one medium. Every method is non-blocking.
    class AbsLink
    {
      public:
        virtual ~AbsLink() = default;

        /// @brief Sends one frame to one peer of this link.
        /// @param data frame bytes
        /// @param size frame size in bytes
        /// @param address peer, as a previous receive() reported it
        /// @return true when the frame was handed to the medium
        virtual bool send(const std::uint8_t *data,
                          std::size_t size,
                          const LinkAddress &address) = 0;

        /// @brief Sends one frame to every peer of this link.
        /// @param data frame bytes
        /// @param size frame size in bytes
        /// @return true when the frame was handed to the medium
        virtual bool broadcast(const std::uint8_t *data, std::size_t size) = 0;

        /// @brief Takes one pending frame, if any, without blocking.
        /// @param[out] bufferOut receives the frame bytes
        /// @param capacity size of bufferOut; a longer frame is dropped
        /// @param[out] fromOut receives the sender's address
        /// @return frame size in bytes, 0 when nothing is pending
        virtual std::size_t receive(std::uint8_t *bufferOut,
                                    std::size_t capacity,
                                    LinkAddress &fromOut) = 0;
    };
} // namespace mark4
