/**
 * The firmware update as the page understands it: the plain words each
 * phase of the gateway's OtaState is shown with, and the rule that says
 * which buttons make sense right now.
 *
 * All of it is pure, and all of it is what the panel is judged on: an update
 * panel that offers "Start" in the middle of a transfer, or "Revert" on a
 * board nobody can reach, is worse than no panel. The gateway is the
 * authority on the state; this file only reads it and decides what is
 * clickable, so both halves can be tested without a DOM.
 */

import { create } from "@bufbuild/protobuf";

import { type OtaState, OtaState_Phase, OtaStateSchema } from "../gen/gateway_pb";
import { OtaSlotState } from "../gen/mark4_pb";

/** What the panel shows before the gateway has said anything. */
export const IDLE_OTA: OtaState = create(OtaStateSchema, { targetSlot: -1 });

/** Phases in which a session occupies the target node. */
const RUNNING = new Set<OtaState_Phase>([
    OtaState_Phase.QUERY,
    OtaState_Phase.ERASING,
    OtaState_Phase.TRANSFER,
    OtaState_Phase.VERIFYING,
    OtaState_Phase.REBOOTING,
    OtaState_Phase.WAITING_BOARD,
    OtaState_Phase.TESTING,
    OtaState_Phase.REVERTING,
]);

/** Phases that end a session one way or the other. */
export const TERMINAL = new Set<OtaState_Phase>([
    OtaState_Phase.CONFIRMED,
    OtaState_Phase.ROLLED_BACK,
    OtaState_Phase.FAILED,
]);

const PHASE_WORDS: Record<OtaState_Phase, string> = {
    [OtaState_Phase.IDLE]: "idle",
    [OtaState_Phase.QUERY]: "asking the board what it runs",
    [OtaState_Phase.ERASING]: "erasing the target slot",
    [OtaState_Phase.TRANSFER]: "sending the image",
    [OtaState_Phase.VERIFYING]: "the board is checking the image",
    [OtaState_Phase.REBOOTING]: "rebooting into the new image",
    [OtaState_Phase.WAITING_BOARD]: "waiting for the board to come back",
    [OtaState_Phase.TESTING]: "the new image runs on trial",
    [OtaState_Phase.CONFIRMED]: "confirmed",
    [OtaState_Phase.ROLLED_BACK]: "rolled back",
    [OtaState_Phase.REVERTING]: "activating the other slot",
    [OtaState_Phase.FAILED]: "failed",
};

const SLOT_STATE_NAMES: Record<OtaSlotState, string> = {
    [OtaSlotState.EMPTY]: "empty",
    [OtaSlotState.STAGED]: "staged",
    [OtaSlotState.TESTING]: "testing",
    [OtaSlotState.VALID]: "valid",
    [OtaSlotState.BAD]: "bad",
};

/** True while a session occupies the target node. */
export function otaRunning(state: OtaState): boolean {
    return RUNNING.has(state.phase);
}

/** The phase in the words an operator reads. */
export function phaseLabel(phase: OtaState_Phase): string {
    return PHASE_WORDS[phase] ?? `phase ${phase}`;
}

/** How a phase should be colored: good, bad, busy or nothing. */
export function phaseTone(state: OtaState): "good" | "bad" | "warn" | "" {
    if (state.phase === OtaState_Phase.CONFIRMED) {
        return "good";
    }
    if (state.phase === OtaState_Phase.FAILED || state.phase === OtaState_Phase.ROLLED_BACK) {
        return "bad";
    }
    return otaRunning(state) ? "warn" : "";
}

/** "A" or "B", or "-" for a slot index the board does not have. */
export function slotLetter(slot: number): string {
    if (slot === 0) {
        return "A";
    }
    if (slot === 1) {
        return "B";
    }
    return "-";
}

/** The 0xFFFFFFFF an image linked but never packaged carries. */
const UNSTAMPED_EPOCH = 0xffffffff;

/** A build epoch as the operator reads it: local date and time. */
export function buildText(buildEpoch: number): string {
    if (buildEpoch === 0) {
        return "no image header";
    }
    if (buildEpoch === UNSTAMPED_EPOCH) {
        return "unpackaged build";
    }
    const stamp = new Date(buildEpoch * 1000);
    const pad = (value: number): string => String(value).padStart(2, "0");
    return (
        `build ${stamp.getFullYear()}-${pad(stamp.getMonth() + 1)}-${pad(stamp.getDate())}` +
        ` ${pad(stamp.getHours())}:${pad(stamp.getMinutes())}:${pad(stamp.getSeconds())}`
    );
}

/** One identity line: "build 2026-08-24 14:03:05 (deadbeef)", the hash left out when unknown. */
export function identityText(buildEpoch: number, gitHash: string): string {
    const build = buildText(buildEpoch);
    return gitHash === "" ? build : `${build} (${gitHash})`;
}

/** A slot state as a word. */
export function slotStateName(state: OtaSlotState): string {
    return SLOT_STATE_NAMES[state] ?? `state ${state}`;
}

/** How a slot state should be colored on its row. */
export function slotTone(state: OtaSlotState): "good" | "bad" | "warn" | "" {
    if (state === OtaSlotState.VALID) {
        return "good";
    }
    if (state === OtaSlotState.BAD) {
        return "bad";
    }
    if (state === OtaSlotState.TESTING || state === OtaSlotState.STAGED) {
        return "warn";
    }
    return "";
}

/** Share of the image the board has written, in [0, 100]. */
export function progressPercent(state: OtaState): number {
    const progress = state.progress;
    if (progress === undefined || progress.totalBytes === 0) {
        return 0;
    }
    return Math.max(0, Math.min(100, (100 * progress.ackedBytes) / progress.totalBytes));
}

/** The bar caption: what the board has written, out of what it was promised. */
export function progressText(state: OtaState): string {
    const progress = state.progress;
    if (progress === undefined || progress.totalBytes === 0) {
        return "";
    }
    const percent = Math.round(progressPercent(state));
    const retries = progress.retries;
    const resent = retries === 0 ? "" : `, ${retries} resend${retries === 1 ? "" : "s"}`;
    return `${progress.ackedBytes} / ${progress.totalBytes} bytes (${percent}%${resent})`;
}

/**
 * The verdict in plain words. The gateway writes the sentence, so both a
 * page and a script read the same one; this only fills in for a gateway that
 * sent none.
 */
export function verdictText(state: OtaState): string {
    if (state.verdictText !== "") {
        return state.verdictText;
    }
    return state.lastError;
}

/** Which of the panel's controls make sense in the current state. */
export interface OtaActions {
    /** Start the update. */
    start: boolean;
    /** Drop the running update. */
    abort: boolean;
    /** Ask the board to go back to its other slot. */
    revert: boolean;
    /** Let the operator type a bundle path. */
    editBundle: boolean;
}

/**
 * A control is offered only when it would do something. `boardLinked` is
 * "the chosen target node is alive": with no node there is nothing to update,
 * and offering the buttons anyway would only produce refusals.
 */
export function otaActions(state: OtaState, boardLinked: boolean): OtaActions {
    const running = otaRunning(state);
    return {
        start: boardLinked && !running,
        abort: running,
        revert: boardLinked && (!running || state.phase === OtaState_Phase.TESTING),
        editBundle: !running,
    };
}
