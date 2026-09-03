#pragma once

/// @file
/// @brief A transport link that keeps every frame handed to it, so a test
///        can check what a service sent, to whom, and decode the payload.
///        Every service that answers on the wire now takes a Transport, so
///        this is the seam the old fake senders used to be.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
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
            if (m_inbound.empty())
            {
                return 0U;
            }
            const std::vector<std::uint8_t> frame = m_inbound.front();
            m_inbound.pop_front();
            if (frame.size() > capacity)
            {
                return 0U;
            }
            std::copy(frame.begin(), frame.end(), bufferOut);
            fromOut = m_peer;
            return frame.size();
        }

        /// @brief Queues one frame for the next receive(), so a test can make
        ///        the transport learn a node and then be able to unicast to
        ///        it: a node the transport never heard from has no address
        ///        and no link, and every send to it is refused.
        /// @param src node the frame comes from
        /// @param dst node it is addressed to
        /// @param payload payload bytes
        void deliver(std::uint32_t src, std::uint32_t dst, const std::vector<std::uint8_t> &payload)
        {
            FrameHeader header;
            header.src = src;
            header.dst = dst;
            header.seq = m_inboundSeq;
            ++m_inboundSeq;
            header.hops = INBOUND_HOPS;
            std::vector<std::uint8_t> frame(FRAME_HEADER_SIZE + payload.size(), 0U);
            encodeFrameHeader(header, frame.data());
            std::copy(payload.begin(), payload.end(), frame.begin() + FRAME_HEADER_SIZE);
            m_inbound.push_back(frame);
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

        /// Hops a queued frame carries: enough that a relay would forward it.
        static constexpr std::uint8_t INBOUND_HOPS = 4U;

        std::vector<RecordedFrame> m_frames;             ///< everything handed over
        std::deque<std::vector<std::uint8_t>> m_inbound; ///< frames queued for receive()
        std::uint16_t m_inboundSeq = 1U;                 ///< sequence of the next queued frame
        LinkAddress m_peer{1U, 2U};                      ///< where queued frames come from
        bool m_accept = true;                            ///< what the medium answers
    };
} // namespace mark4
