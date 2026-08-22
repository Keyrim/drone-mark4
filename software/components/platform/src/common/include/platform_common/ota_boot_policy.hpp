#pragma once

/// @file
/// @brief The two decisions the boot metadata drives outside the updater,
///        per docs/ota-design.md sections 3.2 and 4.5: which slot a reset
///        boots, and whether the running image is still on trial.
///
///        Both are pure functions of an OtaMetaState, deliberately: the
///        bootloader reaches its metadata through a flash log, the desktop
///        flight process through a file-backed store, and neither storage
///        detail belongs in the decision. Reading the metadata and
///        persisting what the decision implies stay with the caller, which
///        is what lets drone_boot and drone_sim run the very same slot
///        choice - the sim's fake trial boot is not an imitation of the
///        bootloader, it is the same code.
///
///        Board-safe: no allocation, no exceptions, no storage access.

#include <cstdint>

#include "platform/firmware_store.hpp"
#include "protocol/ota.hpp"

namespace mark4
{
    /// What one reset resolves to: the slot to run, and whether the state
    /// the decision left behind must be persisted before running it.
    struct OtaBootDecision
    {
        std::uint8_t slot = OTA_SLOT_A; ///< slot to boot
        bool persist = false;           ///< one metadata record must be appended first
    };

    /// @brief Finds a slot in a given lifecycle state.
    /// @param state metadata to search
    /// @param wanted OTA_SLOT_* state to look for
    /// @param[out] slotOut slot found, untouched when none matches
    /// @return true when a slot is in that state
    [[nodiscard]] inline bool otaFindSlotInState(const OtaMetaState &state,
                                                 std::uint8_t wanted,
                                                 std::uint8_t &slotOut)
    {
        for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            if (state.slotState[slot] == wanted)
            {
                slotOut = slot;
                return true;
            }
        }
        return false;
    }

    /// @brief The slot decision of a reset: a staged image gets its one
    ///        trial, a trial that was already spent is rolled back, and
    ///        anything else runs the slot the metadata prefers.
    ///
    ///        The state changes the choice implies are written into
    ///        stateInOut rather than persisted here: the caller appends one
    ///        record when persist comes back true, which is what keeps every
    ///        transition a single atomic metadata write.
    /// @param[in,out] stateInOut metadata as read, updated in place to what
    ///                must be persisted before the chosen slot runs
    /// @return the slot to run and whether a record is owed
    [[nodiscard]] inline OtaBootDecision otaDecideBoot(OtaMetaState &stateInOut)
    {
        std::uint8_t slot = 0U;
        if (otaFindSlotInState(stateInOut, OTA_SLOT_STAGED, slot))
        {
            // The one-shot trial: mark it attempted before booting it, so a
            // firmware that never comes back cannot get a second chance.
            stateInOut.slotState[slot] = OTA_SLOT_TESTING;
            stateInOut.trialAttempted = true;
            return {slot, true};
        }

        if (otaFindSlotInState(stateInOut, OTA_SLOT_TESTING, slot))
        {
            if (stateInOut.trialAttempted)
            {
                // It was booted once and never confirmed: roll back.
                stateInOut.slotState[slot] = OTA_SLOT_BAD;
                stateInOut.trialAttempted = false;
                return {stateInOut.activeSlot, true};
            }
            // TESTING without the attempted flag can only be a sequence torn
            // by a power cut between the two records. The image is staged and
            // untried: treat it as STAGED and spend the trial now.
            stateInOut.trialAttempted = true;
            return {slot, true};
        }

        return {stateInOut.activeSlot, false};
    }

    /// @brief The arming interlock of docs/ota-design.md section 3.2: a
    ///        firmware that has not yet proven its link may not take the
    ///        drone into the air, so arming is refused for as long as the
    ///        running slot sits on trial.
    /// @param state metadata as read
    /// @param runningSlot slot the running image executes from
    /// @return true while arming must be refused
    [[nodiscard]] inline bool otaTrialUnconfirmed(const OtaMetaState &state,
                                                  std::uint8_t runningSlot)
    {
        return runningSlot < OTA_SLOT_COUNT && state.slotState[runningSlot] == OTA_SLOT_TESTING;
    }
} // namespace mark4
