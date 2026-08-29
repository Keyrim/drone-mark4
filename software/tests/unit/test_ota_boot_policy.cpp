/// @file
/// @brief The slot decision every reset runs, and the arming interlock that
///        goes with it (docs/ota-design.md sections 3.2 and 4.5). It is a
///        pure function of the boot metadata on purpose: the bootloader and
///        the desktop flight process share it, so these cases are the ones
///        behind "an image that never comes back gets exactly one chance".

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "platform/firmware_store.hpp"
#include "platform_common/ota_boot_policy.hpp"
#include "protocol/ota_image.hpp"

TEST_CASE("a blank board boots the slot its metadata prefers, and owes no record")
{
    mark4::OtaMetaState state;
    const mark4::OtaBootDecision decision = mark4::otaDecideBoot(state);

    REQUIRE(decision.slot == mark4::OTA_SLOT_A);
    REQUIRE(!decision.persist);
    REQUIRE(state.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
    REQUIRE(!state.trialAttempted);
}

TEST_CASE("a staged image is booted once, marked attempted before it runs")
{
    mark4::OtaMetaState state;
    state.activeSlot = mark4::OTA_SLOT_A;
    state.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED};

    const mark4::OtaBootDecision decision = mark4::otaDecideBoot(state);

    // The attempted flag is set in the record written BEFORE the jump: that
    // ordering is the whole one-shot guarantee.
    REQUIRE(decision.slot == mark4::OTA_SLOT_B);
    REQUIRE(decision.persist);
    REQUIRE(state.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_TESTING);
    REQUIRE(state.trialAttempted);
    // Staging never moves the active slot, which is what makes the rollback
    // free: the fallback is still the slot the metadata prefers.
    REQUIRE(state.activeSlot == mark4::OTA_SLOT_A);
}

TEST_CASE("a trial that was already spent is marked bad and the old image comes back")
{
    mark4::OtaMetaState state;
    state.activeSlot = mark4::OTA_SLOT_A;
    state.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_TESTING};
    state.trialAttempted = true;

    const mark4::OtaBootDecision decision = mark4::otaDecideBoot(state);

    REQUIRE(decision.slot == mark4::OTA_SLOT_A);
    REQUIRE(decision.persist);
    REQUIRE(state.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_BAD);
    REQUIRE(!state.trialAttempted);
}

TEST_CASE("a torn staging sequence spends the trial it never got")
{
    // TESTING without the attempted flag can only be a power cut between the
    // two records of a trial boot: the image is staged and untried.
    mark4::OtaMetaState state;
    state.activeSlot = mark4::OTA_SLOT_B;
    state.slotState = {mark4::OTA_SLOT_TESTING, mark4::OTA_SLOT_VALID};
    state.trialAttempted = false;

    const mark4::OtaBootDecision decision = mark4::otaDecideBoot(state);

    REQUIRE(decision.slot == mark4::OTA_SLOT_A);
    REQUIRE(decision.persist);
    REQUIRE(state.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_TESTING);
    REQUIRE(state.trialAttempted);
}

TEST_CASE("a confirmed trial is an ordinary boot of the active slot")
{
    mark4::OtaMetaState state;
    state.activeSlot = mark4::OTA_SLOT_B;
    state.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_VALID};

    const mark4::OtaBootDecision decision = mark4::otaDecideBoot(state);

    REQUIRE(decision.slot == mark4::OTA_SLOT_B);
    REQUIRE(!decision.persist);
}

TEST_CASE("a bad slot is never chosen again, however the metadata got there")
{
    mark4::OtaMetaState state;
    state.activeSlot = mark4::OTA_SLOT_A;
    state.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_BAD};

    const mark4::OtaBootDecision decision = mark4::otaDecideBoot(state);

    REQUIRE(decision.slot == mark4::OTA_SLOT_A);
    REQUIRE(!decision.persist);
    REQUIRE(state.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_BAD);
}

TEST_CASE("staging wins over a trial of the other slot: the newest image gets the chance")
{
    mark4::OtaMetaState state;
    state.activeSlot = mark4::OTA_SLOT_A;
    state.slotState = {mark4::OTA_SLOT_TESTING, mark4::OTA_SLOT_STAGED};
    state.trialAttempted = true;

    const mark4::OtaBootDecision decision = mark4::otaDecideBoot(state);

    REQUIRE(decision.slot == mark4::OTA_SLOT_B);
    REQUIRE(state.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_TESTING);
}

TEST_CASE("arming is refused while the running slot is on trial, and only then")
{
    mark4::OtaMetaState state;
    state.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_TESTING};

    REQUIRE(mark4::otaTrialUnconfirmed(state, mark4::OTA_SLOT_B));
    // The interlock is about the image that RUNS: a trial sitting in the
    // other slot is not flying anything.
    REQUIRE(!mark4::otaTrialUnconfirmed(state, mark4::OTA_SLOT_A));

    state.slotState[mark4::OTA_SLOT_B] = mark4::OTA_SLOT_VALID;
    REQUIRE(!mark4::otaTrialUnconfirmed(state, mark4::OTA_SLOT_B));

    // A slot index this system does not have can never gate anything, so it
    // cannot read past the two-slot array either.
    REQUIRE(!mark4::otaTrialUnconfirmed(state, mark4::OTA_SLOT_COUNT));
}
