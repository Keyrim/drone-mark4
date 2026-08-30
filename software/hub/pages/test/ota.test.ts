import assert from "node:assert/strict";
import test from "node:test";

import { create } from "@bufbuild/protobuf";

import { type OtaState, OtaState_Phase, OtaStateSchema } from "../src/gen/gateway_pb";
import { OtaSlotState } from "../src/gen/mark4_pb";
import {
    IDLE_OTA,
    buildText,
    identityText,
    otaActions,
    otaRunning,
    phaseLabel,
    phaseTone,
    progressPercent,
    progressText,
    slotLetter,
    slotStateName,
    slotTone,
    verdictText,
} from "../src/console/ota";

/** One transfer in progress, as the gateway publishes it. */
const TRANSFER: OtaState = create(OtaStateSchema, {
    phase: OtaState_Phase.TRANSFER,
    targetNode: 42,
    targetSlot: 1,
    bundle: {
        loaded: true,
        path: "software/build/stm32/drone_firmware/drone_firmware.ota",
        name: "drone_firmware",
        mcu: 1,
        buildEpoch: 1756100000,
        gitHash: "bbbbbbbb",
        wireHash: "7e8201a9",
        imageCount: 2,
    },
    board: {
        seen: true,
        runningSlot: 0,
        activeSlot: 0,
        slots: [
            { state: OtaSlotState.VALID, buildEpoch: 1756000000, gitHash: "aaaaaaaa" },
            { state: OtaSlotState.EMPTY, buildEpoch: 0, gitHash: "" },
        ],
        slotSize: 393216,
        maxChunkData: 240,
    },
    progress: { sentBytes: 3840, ackedBytes: 1920, totalBytes: 8512, retries: 0 },
});

/** @returns the idle state with a phase and a few fields overridden. */
function withPhase(phase: OtaState_Phase, extra: Partial<OtaState> = {}): OtaState {
    return create(OtaStateSchema, { ...IDLE_OTA, phase, ...extra });
}

test("the idle state is what a page assumes before the gateway speaks", () => {
    assert.equal(IDLE_OTA.phase, OtaState_Phase.IDLE);
    assert.equal(IDLE_OTA.targetSlot, -1);
    assert.equal(otaRunning(IDLE_OTA), false);
    assert.equal(progressText(IDLE_OTA), "");
    assert.equal(verdictText(IDLE_OTA), "");
});

test("the running phases are exactly the ones that occupy the node", () => {
    for (const phase of [
        OtaState_Phase.QUERY,
        OtaState_Phase.ERASING,
        OtaState_Phase.TRANSFER,
        OtaState_Phase.VERIFYING,
        OtaState_Phase.REBOOTING,
        OtaState_Phase.WAITING_BOARD,
        OtaState_Phase.TESTING,
        OtaState_Phase.REVERTING,
    ]) {
        assert.equal(otaRunning(withPhase(phase)), true, phaseLabel(phase));
    }
    for (const phase of [
        OtaState_Phase.IDLE,
        OtaState_Phase.CONFIRMED,
        OtaState_Phase.ROLLED_BACK,
        OtaState_Phase.FAILED,
    ]) {
        assert.equal(otaRunning(withPhase(phase)), false, phaseLabel(phase));
    }
});

test("every phase reads as plain words, never as an identifier", () => {
    assert.equal(phaseLabel(OtaState_Phase.ERASING), "erasing the target slot");
    assert.equal(phaseLabel(OtaState_Phase.WAITING_BOARD), "waiting for the board to come back");
    assert.equal(phaseLabel(OtaState_Phase.ROLLED_BACK), "rolled back");
    assert.notEqual(phaseLabel(OtaState_Phase.TESTING), "testing");
});

test("the phase is colored good when confirmed and bad when it went wrong", () => {
    assert.equal(phaseTone(withPhase(OtaState_Phase.CONFIRMED)), "good");
    assert.equal(phaseTone(withPhase(OtaState_Phase.FAILED)), "bad");
    assert.equal(phaseTone(withPhase(OtaState_Phase.ROLLED_BACK)), "bad");
    assert.equal(phaseTone(TRANSFER), "warn");
    assert.equal(phaseTone(IDLE_OTA), "");
});

test("the progress bar follows the bytes the board acknowledged", () => {
    assert.ok(Math.abs(progressPercent(TRANSFER) - 22.55) < 0.01);
    assert.equal(progressText(TRANSFER), "1920 / 8512 bytes (23%)");
    const resent = create(OtaStateSchema, { ...TRANSFER, progress: { ...TRANSFER.progress!, retries: 2 } });
    assert.equal(progressText(resent), "1920 / 8512 bytes (23%, 2 resends)");
});

test("slots read as letters, words and tones", () => {
    assert.equal(slotLetter(0), "A");
    assert.equal(slotLetter(1), "B");
    assert.equal(slotLetter(7), "-");
    assert.equal(slotStateName(OtaSlotState.VALID), "valid");
    assert.equal(slotTone(OtaSlotState.VALID), "good");
    assert.equal(slotTone(OtaSlotState.BAD), "bad");
    assert.equal(slotTone(OtaSlotState.TESTING), "warn");
    assert.equal(slotTone(OtaSlotState.EMPTY), "");
    assert.equal(buildText(0), "no image header");
    assert.equal(buildText(0xffffffff), "unpackaged build");
    assert.match(identityText(1756100000, "bbbbbbbb"), /^build \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \(bbbbbbbb\)$/);
    assert.match(identityText(1756100000, ""), /^build \d{4}/);
});

test("the verdict sentence comes from the gateway, the error fills in", () => {
    assert.equal(verdictText(withPhase(OtaState_Phase.FAILED, { lastError: "no answer" })), "no answer");
    assert.equal(
        verdictText(withPhase(OtaState_Phase.CONFIRMED, { verdictText: "the new firmware runs", lastError: "x" })),
        "the new firmware runs"
    );
});

test("nothing is clickable without a target node", () => {
    const actions = otaActions(TRANSFER, false);
    assert.equal(actions.start, false);
    assert.equal(actions.revert, false);
});

test("a running transfer offers only the abort", () => {
    const actions = otaActions(TRANSFER, true);
    assert.equal(actions.start, false);
    assert.equal(actions.abort, true);
    assert.equal(actions.editBundle, false);
});

test("an idle board with a target offers the update and the revert", () => {
    const actions = otaActions(IDLE_OTA, true);
    assert.equal(actions.start, true);
    assert.equal(actions.abort, false);
    assert.equal(actions.revert, true);
    assert.equal(actions.editBundle, true);
});

test("a trial image can be reverted without aborting first", () => {
    const actions = otaActions(withPhase(OtaState_Phase.TESTING), true);
    assert.equal(actions.revert, true);
    assert.equal(actions.abort, true);
    assert.equal(actions.start, false);
});
