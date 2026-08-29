#include "transport/uart_link.hpp"

#include <cstring>

namespace mark4
{
    bool UartLink::send(const std::uint8_t *data, std::size_t size, const LinkAddress &address)
    {
        static_cast<void>(address); // point to point: there is only one peer
        const std::size_t frameSize = encodeSerialFrame(data, size, m_txFrame.data());
        if (frameSize == 0U)
        {
            return false;
        }
        return m_stream.write(m_txFrame.data(), frameSize);
    }

    bool UartLink::broadcast(const std::uint8_t *data, std::size_t size)
    {
        return send(data, size, LinkAddress{});
    }

    std::size_t UartLink::receive(std::uint8_t *bufferOut,
                                  std::size_t capacity,
                                  LinkAddress &fromOut)
    {
        fromOut = LinkAddress{};
        for (;;)
        {
            while (m_pendingIndex < m_pendingSize)
            {
                const std::size_t frameSize = m_parser.feed(m_pending[m_pendingIndex]);
                ++m_pendingIndex;
                if (frameSize == 0U)
                {
                    continue;
                }
                if (frameSize > capacity)
                {
                    continue; // too large for the caller: dropped, keep hunting
                }
                std::memcpy(bufferOut, m_parser.payload(), frameSize);
                return frameSize;
            }
            m_pendingSize = m_stream.read(m_pending.data(), m_pending.size());
            m_pendingIndex = 0U;
            if (m_pendingSize == 0U)
            {
                return 0U;
            }
        }
    }
} // namespace mark4
