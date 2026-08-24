import assert from "node:assert/strict";
import test from "node:test";

import {
    IDLE_OTA,
    buildText,
    identityText,
    otaActions,
    otaRunning,
    phaseLabel,
    phaseTone,
    progressText,
    readOtaState,
    slotLetter,
    slotTone,
    verdictText,
    type OtaState,
} from "../src/console/ota";

/** One transfer in progress, as the hub publishes it. */
const TRANSFER_MESSAGE = {
    type: "ota",
    phase: "transfer",
    verdict: "none",
    verdictText: "",
    lastError: "",
    autoConfirm: true,
    confirmReady: false,
    targetSlot: 1,
    bundle: {
        loaded: true,
        path: "software/build/stm32/drone_firmware/drone_firmware.ota",
        name: "drone_firmware",
        mcuId: 1,
        buildEpoch: 1756100000,
        gitHash: "bbbbbbbb",
        protocolVersion: 12,
        images: [
            { slot: 0, size: 8512, crc32: 111 },
            { slot: 1, size: 8512, crc32: 222 },
        ],
    },
    board: {
        seen: true,
        mcuId: 1,
        runningSlot: 0,
        activeSlot: 0,
        updaterBusy: false,
        slots: [
            { state: 3, stateName: "valid", buildEpoch: 1756000000, gitHash: "aaaaaaaa" },
            { state: 255, stateName: "empty", buildEpoch: 0, gitHash: "" },
        ],
        slotSize: 393216,
        maxChunkData: 240,
    },
    progress: { sentBytes: 3840, ackedBytes: 1920, totalBytes: 8512, retries: 0, percent: 22.5 },
};

/** @returns the idle state with a few fields overridden. */
function withPhase(phase: OtaState["phase"], extra: Partial<OtaState> = {}): OtaState {
    return { ...IDLE_OTA, phase, ...extra };
}

test("a transfer message decodes into every field the panel paints", () => {
    const state = readOtaState(TRANSFER_MESSAGE);
    assert.equal(state.phase, "transfer");
    assert.equal(state.verdict, "none");
    assert.equal(state.targetSlot, 1);
    assert.equal(state.bundle.loaded, true);
    assert.equal(state.bundle.buildEpoch, 1756100000);
    assert.equal(state.bundle.gitHash, "bbbbbbbb");
    assert.equal(state.bundle.images.length, 2);
    assert.equal(state.bundle.images[1]?.size, 8512);
    assert.equal(state.board.slots[0]?.buildEpoch, 1756000000);
    assert.equal(state.board.slots[0]?.stateName, "valid");
    assert.equal(state.board.slots[1]?.stateName, "empty");
    assert.equal(state.progress.ackedBytes, 1920);
    assert.equal(state.progress.totalBytes, 8512);
});

test("a hub that says nothing leaves the panel at its idle defaults", () => {
    const state = readOtaState({ type: "ota" });
    assert.deepEqual(state, IDLE_OTA);
});

test("a phase the page does not know falls back to idle rather than breaking", () => {
    const state = readOtaState({ type: "ota", phase: "somethingNewer" });
    assert.equal(state.phase, "idle");
    assert.equal(state.verdict, "none");
});

test("the running phases are exactly the ones that occupy the link", () => {
    for (const phase of [
        "query",
        "erasing",
        "transfer",
        "verifying",
        "rebooting",
        "waitingBoard",
        "testing",
        "reverting",
    ] as OtaState["phase"][]) {
        assert.equal(otaRunning(withPhase(phase)), true, phase);
    }
    for (const phase of ["idle", "confirmed", "rolledBack", "failed"] as OtaState["phase"][]) {
        assert.equal(otaRunning(withPhase(phase)), false, phase);
    }
});

test("every phase reads as plain words, never as an identifier", () => {
    assert.equal(phaseLabel("erasing"), "erasing the target slot");
    assert.equal(phaseLabel("waitingBoard"), "waiting for the board to come back");
    assert.equal(phaseLabel("rolledBack"), "rolled back");
    assert.notEqual(phaseLabel("testing"), "testing");
});

test("the phase is colored good when confirmed and bad when it went wrong", () => {
    assert.equal(phaseTone(withPhase("confirmed")), "good");
    assert.equal(phaseTone(withPhase("failed")), "bad");
    assert.equal(phaseTone(withPhase("rolledBack")), "bad");
    assert.equal(phaseTone(withPhase("transfer")), "warn");
    assert.equal(phaseTone(withPhase("idle")), "");
});

test("nothing is clickable without a board link", () => {
    const actions = otaActions(readOtaState(TRANSFER_MESSAGE), false);
    assert.equal(actions.start, false);
    assert.equal(actions.revert, false);
});

test("a running transfer offers only the abort", () => {
    const actions = otaActions(readOtaState(TRANSFER_MESSAGE), true);
    assert.equal(actions.start, false);
    assert.equal(actions.abort, true);
    assert.equal(actions.editBundle, false);
});

test("an idle board with a link offers the update and the revert", () => {
    const actions = otaActions(withPhase("idle"), true);
    assert.equal(actions.start, true);
    assert.equal(actions.abort, false);
    assert.equal(actions.revert, true);
    assert.equal(actions.editBundle, true);
});

test("a trial image can be reverted without aborting first", () => {
    const actions = otaActions(withPhase("testing"), true);
    assert.equal(actions.revert, true);
    assert.equal(actions.abort, true);
    assert.equal(actions.start, false);
});

test("the bar caption counts written bytes and names the resends", () => {
    const state = readOtaState(TRANSFER_MESSAGE);
    assert.equal(progressText(state), "1920 / 8512 bytes (23%)");
    assert.equal(
        progressText({ ...state, progress: { ...state.progress, retries: 1 } }),
        "1920 / 8512 bytes (23%, 1 resend)"
    );
    assert.equal(
        progressText({ ...state, progress: { ...state.progress, retries: 3 } }),
        "1920 / 8512 bytes (23%, 3 resends)"
    );
    assert.equal(progressText(IDLE_OTA), "");
});

test("an identity leaves out a hash it does not have", () => {
    // 2001-09-09 01:46:40 UTC; rendered in local time, so only the shape
    // and the hash handling are asserted here.
    assert.equal(identityText(1000000000, "deadbeef"), `${buildText(1000000000)} (deadbeef)`);
    assert.equal(identityText(1000000000, ""), buildText(1000000000));
    assert.match(buildText(1000000000), /^build \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);
    assert.equal(buildText(0), "no image header");
    assert.equal(buildText(0xffffffff), "unpackaged build");
});

test("a slot state maps to the tone its row is painted with", () => {
    assert.equal(slotTone("valid"), "good");
    assert.equal(slotTone("bad"), "bad");
    assert.equal(slotTone("testing"), "warn");
    assert.equal(slotTone("staged"), "warn");
    assert.equal(slotTone("empty"), "");
    assert.equal(slotLetter(0), "A");
    assert.equal(slotLetter(1), "B");
    assert.equal(slotLetter(7), "-");
});

test("the verdict shown is the hub's sentence, the error only as a fallback", () => {
    assert.equal(
        verdictText(withPhase("confirmed", { verdictText: "update confirmed: the board runs X." })),
        "update confirmed: the board runs X."
    );
    assert.equal(
        verdictText(withPhase("failed", { verdictText: "", lastError: "cannot read 'x.ota'" })),
        "cannot read 'x.ota'"
    );
    assert.equal(verdictText(IDLE_OTA), "");
});
