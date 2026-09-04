#pragma once

/// @file
/// @brief The whole bootloader: read the boot metadata, decide which slot
///        to run, validate it, jump. See docs/ota-design.md section 4.5.
///
///        What it deliberately does not do: no flight core, no UART, no
///        interrupts enabled, no watchdog started, and not one register of
///        the clock tree touched. It runs on the 16 MHz internal oscillator
///        the reset state leaves it on and hands the firmware a chip that
///        looks freshly reset, which is the only clock state the firmware's
///        own initSystemClock() is written against. The cost is a slower
///        software CRC; images are tens of kilobytes, so that is tens of
///        milliseconds once per boot.
///
///        Its interface to the rest of the system is the metadata format and
///        the image header, nothing else: no shared RAM, no arguments, no
///        protocol. That is what allows the slot strategy to change later
///        without moving anything outside this directory.

#include <cstdint>

#include "ota/firmware_store.hpp"
#include "ota/meta_log.hpp"
#include "platform_stm32/ota_meta_flash.hpp"

namespace mark4
{
    class BootApp
    {
      public:
        /// @brief Runs the boot decision and jumps into the chosen image.
        ///        Never returns: either a firmware slot takes over, or the
        ///        LED pattern of a board with nothing bootable runs forever.
        [[noreturn]] void run();

      private:
        /// @brief Picks the slot to boot and persists the state changes that
        ///        choice implies (a trial marked attempted, a failed trial
        ///        marked bad).
        /// @param[out] state metadata as read, then updated in place as the
        ///             records this decision implies are appended
        /// @return slot to try first
        std::uint8_t chooseSlot(OtaMetaState &state);

        /// @brief Checks an image against its own header, and against its
        ///        checksums when the packaging script stamped them.
        /// @param slot slot to validate
        /// @return true when the image may be jumped to
        [[nodiscard]] static bool Validates(std::uint8_t slot);

        /// @brief Appends one record marking a slot bad. Best effort: a
        ///        metadata write that fails must not stop the fallback from
        ///        booting, it only costs one repeated trial next time.
        /// @param state metadata to update and persist
        /// @param slot slot to mark OTA_SLOT_BAD
        void markBad(OtaMetaState &state, std::uint8_t slot);

        /// @brief Hands the core over to a firmware image: interrupts off,
        ///        VTOR at the image's vector table, main stack pointer from
        ///        vector 0, branch to vector 1.
        /// @param slot slot to boot
        [[noreturn]] static void JumpToSlot(std::uint8_t slot);

        /// @brief Blinks the "nothing bootable" pattern forever and waits
        ///        for a debug probe. The only exit is SWD or a power cycle.
        [[noreturn]] static void PanicBlink();

        /// Declaration order is construction order: the backend exists
        /// before the log that binds a reference to it.
        OtaMetaFlashBackend m_metaBackend{};                      ///< the two metadata sectors
        OtaMetaLog<OtaMetaFlashBackend> m_metaLog{m_metaBackend}; ///< boot metadata log
    };
} // namespace mark4
