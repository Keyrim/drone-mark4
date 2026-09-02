#pragma once

/// @file
/// @brief A transport link that keeps every frame handed to it, so a test
///        can check what a service sent, to whom, and decode the payload.
///        Every service that answers on the wire now takes a Transport, so
///        this is the seam the old fake senders used to be.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "protocol/envelope.hpp"
#include "transport/frame.hpp"
#include "transport/link.hpp"

namespace mark4
{
    /// One frame the link was asked to move, header decoded.
    struct RecordedFrame
    {
        FrameHeader header;                ///< src, dst, seq, hops as encoded
        std::vector<std::uint8_t> payload; ///< bytes behind the header
        bool broadcast = false;            ///< handed to broadcast(), not send()
        LinkAddress address;               ///< peer of a unicast, empty otherwise
    };

    /// Link recording everything, receiving nothing. Allocates freely: this
    /// is a test.
    class RecordingLink final : public AbsLink
    {
      public:
        bool send(const std::uint8_t *data, std::size_t size, const LinkAddress &address) override
        {
            record(data, size, false, address);
            return m_accept;
        }

        bool broadcast(const std::uint8_t *data, std::size_t size) override
        {
            record(data, size, true, LinkAddress{});
            return m_accept;
        }

        std::size_t receive(std::uint8_t *bufferOut,
                            std::size_t capacity,
                            LinkAddress &fromOut) override
        {
            static_cast<void>(bufferOut);
            static_cast<void>(capacity);
            static_cast<void>(fromOut);
            return 0U;
        }

        /// @brief Decides what the medium answers from now on.
        /// @param accept true to take frames, false to refuse them
        void setAccept(bool accept)
        {
            m_accept = accept;
        }

        /// @return frames recorded since construction, in order
        [[nodiscard]] const std::vector<RecordedFrame> &frames() const
        {
            return m_frames;
        }

        /// @brief Forgets everything recorded so far.
        void clear()
        {
            m_frames.clear();
        }

        /// @param index frame to decode, 0 <= index < frames().size()
        /// @return the Envelope that frame carries, empty when it holds none
        [[nodiscard]] std::optional<mark4_Envelope> envelope(std::size_t index) const
        {
            const RecordedFrame &frame = m_frames[index];
            mark4_Envelope decoded;
            if (!decodeEnvelope(frame.payload.data(), frame.payload.size(), decoded))
            {
                return std::nullopt;
            }
            return decoded;
        }

      private:
        /// @brief Decodes and stores one frame.
        /// @param data frame bytes
        /// @param size frame size in bytes
        /// @param broadcast true when it came from broadcast()
        /// @param address peer of a unicast
        void record(const std::uint8_t *data,
                    std::size_t size,
                    bool broadcast,
                    const LinkAddress &address)
        {
            RecordedFrame frame;
            if (!decodeFrameHeader(data, size, frame.header))
            {
                return;
            }
            frame.payload.assign(data + FRAME_HEADER_SIZE, data + size);
            frame.broadcast = broadcast;
            frame.address = address;
            m_frames.push_back(frame);
        }

        std::vector<RecordedFrame> m_frames; ///< everything handed over
        bool m_accept = true;                ///< what the medium answers
    };
} // namespace mark4
