#pragma once

/// @file
/// @brief Command receiver fed by the transport: the composition root hands
///        it every payload Transport::poll() delivers, and the consumers
///        take them one per poll() as the interface promises. A fixed ring,
///        no allocation: a burst beyond its capacity drops the oldest.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "platform/command_receiver.hpp"
#include "protocol/envelope.hpp"
#include "transport/frame.hpp"

namespace mark4
{
    static_assert(MAX_ENVELOPE_SIZE <= MAX_PAYLOAD, "every Envelope must fit one transport frame");

    class CommandReceiverTransport final : public AbsCommandReceiver
    {
      public:
        /// Payloads held between two drains. The uplink carries a few
        /// packets per flight frame at most; an OTA burst is paced by its
        /// acknowledgement window, which is what this must hold.
        static constexpr std::size_t CAPACITY = 32U;

        /// @brief Queues one payload the transport delivered.
        /// @param payload payload bytes
        /// @param size payload size, at most MAX_PAYLOAD
        void push(const std::uint8_t *payload, std::size_t size)
        {
            if (size == 0U || size > MAX_PAYLOAD)
            {
                return;
            }
            if (m_count == CAPACITY)
            {
                m_head = (m_head + 1U) % CAPACITY;
                --m_count;
                ++m_dropped;
            }
            const std::size_t tail = (m_head + m_count) % CAPACITY;
            std::memcpy(m_slots[tail].data(), payload, size);
            m_sizes[tail] = size;
            ++m_count;
        }

        /// @brief Hands out the oldest queued payload.
        /// @param[out] bufferOut destination, valid only when returning > 0
        /// @param capacity size of the destination buffer in bytes
        /// @return payload size, 0 when nothing is queued or it does not fit
        std::size_t poll(std::uint8_t *bufferOut, std::size_t capacity) override
        {
            if (m_count == 0U)
            {
                return 0U;
            }
            const std::size_t size = m_sizes[m_head];
            const std::uint8_t *slot = m_slots[m_head].data();
            m_head = (m_head + 1U) % CAPACITY;
            --m_count;
            if (size > capacity)
            {
                ++m_dropped;
                return 0U;
            }
            std::memcpy(bufferOut, slot, size);
            ++m_packetsReceived;
            return size;
        }

        /// @return payloads handed to the consumer since construction
        [[nodiscard]] std::uint32_t packetsReceived() const
        {
            return m_packetsReceived;
        }

        /// @return payloads lost to a full ring or a too small buffer
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

      private:
        std::array<std::array<std::uint8_t, MAX_PAYLOAD>, CAPACITY> m_slots{}; ///< ring storage
        std::array<std::size_t, CAPACITY> m_sizes{}; ///< payload size per slot
        std::size_t m_head = 0U;                     ///< oldest queued slot
        std::size_t m_count = 0U;                    ///< slots in use
        std::uint32_t m_packetsReceived = 0U;        ///< payloads handed out
        std::uint32_t m_dropped = 0U;                ///< payloads lost
    };
} // namespace mark4
