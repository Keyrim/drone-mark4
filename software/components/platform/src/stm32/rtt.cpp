#include "platform_stm32/rtt.hpp"

#include <cstdint>
#include <cstring>

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t UP_BUFFER_SIZE = 1024U;
        constexpr std::uint32_t DOWN_BUFFER_SIZE = 16U;

        /// RTT operating mode 0: drop the whole write when it does not fit.
        constexpr std::uint32_t MODE_NO_BLOCK_SKIP = 0U;

        /// Target-to-host buffer descriptor. Field order and widths are
        /// fixed by the RTT protocol (SEGGER_RTT_BUFFER_UP): the probe
        /// parses this struct byte by byte.
        struct RttBufferUp
        {
            const char *name;             ///< display name on the host
            char *buffer;                 ///< ring storage
            std::uint32_t size;           ///< ring storage size in bytes
            std::uint32_t wrOff;          ///< write offset, target-owned
            volatile std::uint32_t rdOff; ///< read offset, host-owned
            std::uint32_t flags;          ///< operating mode
        };

        /// Host-to-target buffer descriptor (SEGGER_RTT_BUFFER_DOWN).
        struct RttBufferDown
        {
            const char *name;             ///< display name on the host
            char *buffer;                 ///< ring storage
            std::uint32_t size;           ///< ring storage size in bytes
            volatile std::uint32_t wrOff; ///< write offset, host-owned
            std::uint32_t rdOff;          ///< read offset, target-owned
            std::uint32_t flags;          ///< operating mode
        };

        /// Control block header (SEGGER_RTT_CB): the 16-byte id is what the
        /// probe scans RAM for.
        struct RttControlBlock
        {
            char id[16];                 ///< "SEGGER RTT" zero-padded
            std::int32_t maxUpBuffers;   ///< entries in up[]
            std::int32_t maxDownBuffers; ///< entries in down[]
            RttBufferUp up[1];           ///< target-to-host channels
            RttBufferDown down[1];       ///< host-to-target channels
        };

        char g_upData[UP_BUFFER_SIZE];
        char g_downData[DOWN_BUFFER_SIZE];
        RttControlBlock g_rtt;

        /// @brief Queues raw bytes on up buffer 0, dropping the whole write
        ///        when the ring lacks space (mode NO_BLOCK_SKIP).
        /// @param data bytes to queue
        /// @param length number of bytes
        void writeBytes(const char *data, std::uint32_t length)
        {
            RttBufferUp &up = g_rtt.up[0];
            const std::uint32_t wrOff = up.wrOff;
            const std::uint32_t rdOff = up.rdOff;
            const std::uint32_t freeBytes =
                (rdOff > wrOff) ? (rdOff - wrOff - 1U) : (up.size - wrOff + rdOff - 1U);
            if (length > freeBytes)
            {
                return;
            }

            std::uint32_t offset = wrOff;
            for (std::uint32_t index = 0U; index < length; ++index)
            {
                up.buffer[offset] = data[index];
                offset = (offset + 1U == up.size) ? 0U : offset + 1U;
            }
            // The probe reads asynchronously: the data must be in the ring
            // before the write offset moves.
            __asm volatile("" ::: "memory");
            up.wrOff = offset;
        }
    } // namespace

    void rttInit()
    {
        g_rtt.maxUpBuffers = 1;
        g_rtt.maxDownBuffers = 1;

        g_rtt.up[0].name = "Terminal";
        g_rtt.up[0].buffer = g_upData;
        g_rtt.up[0].size = UP_BUFFER_SIZE;
        g_rtt.up[0].wrOff = 0U;
        g_rtt.up[0].rdOff = 0U;
        g_rtt.up[0].flags = MODE_NO_BLOCK_SKIP;

        g_rtt.down[0].name = "Terminal";
        g_rtt.down[0].buffer = g_downData;
        g_rtt.down[0].size = DOWN_BUFFER_SIZE;
        g_rtt.down[0].wrOff = 0U;
        g_rtt.down[0].rdOff = 0U;
        g_rtt.down[0].flags = MODE_NO_BLOCK_SKIP;

        // The id is written last and in two pieces: the descriptors are
        // valid before the magic appears, and the full magic string never
        // sits contiguous in flash where a wide probe scan could latch on
        // the image instead of this block.
        std::memset(g_rtt.id, 0, sizeof(g_rtt.id));
        __asm volatile("" ::: "memory");
        std::memcpy(&g_rtt.id[0], "SEGG", 4U);
        std::memcpy(&g_rtt.id[4], "ER RTT", 6U);
    }

    void rttWrite(const char *text)
    {
        writeBytes(text, static_cast<std::uint32_t>(std::strlen(text)));
    }
} // namespace mark4
