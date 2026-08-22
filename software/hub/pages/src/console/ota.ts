/**
 * The firmware update as the page understands it: the shape of the hub's
 * `ota` message, the plain words each phase is shown with, and the rule that
 * says which buttons make sense right now.
 *
 * All of it is pure, and all of it is what the panel is judged on: an update
 * panel that offers "Start" in the middle of a transfer, or "Confirm" on a
 * board that runs nothing on trial, is worse than no panel. The hub is the
 * authority on the state; this file only reads it and decides what is
 * clickable, so both halves can be tested without a DOM.
 */

/** Phases the hub reports, in the order one session walks them. */
export type OtaPhase =
    | "idle"
    | "query"
    | "erasing"
    | "transfer"
    | "verifying"
    | "rebooting"
    | "waitingBoard"
    | "testing"
    | "confirmed"
    | "rolledBack"
    | "reverting"
    | "failed";

/** Outcome of the last finished session. */
export type OtaVerdict = "none" | "confirmed" | "rolledBack" | "reverted" | "failed";

/** One image of the bundle, as the manifest describes it. */
export interface OtaImageInfo {
    slot: number;
    size: number;
    crc32: number;
}

/** What the loaded bundle holds. */
export interface OtaBundleInfo {
    loaded: boolean;
    /** Path a start would use, whether or not it loaded. */
    path: string;
    name: string;
    mcuId: number;
    version: string;
    gitHash: string;
    protocolVersion: number;
    images: OtaImageInfo[];
}

/** What the board last said about itself. */
export interface OtaBoardInfo {
    seen: boolean;
    mcuId: number;
    runningSlot: number;
    slotState: number[];
    slotStateNames: string[];
    updaterBusy: boolean;
    version: string;
    gitHash: string;
    slotSize: number;
    maxChunkData: number;
}

/** How far the transfer has got. */
export interface OtaProgressInfo {
    sentBytes: number;
    /** What the board acknowledged writing: this is what the bar shows. */
    ackedBytes: number;
    totalBytes: number;
    retries: number;
    percent: number;
}

/** The whole update state, one `ota` message decoded. */
export interface OtaState {
    phase: OtaPhase;
    verdict: OtaVerdict;
    verdictText: string;
    lastError: string;
    autoConfirm: boolean;
    confirmReady: boolean;
    /** Slot being filled, -1 outside a session. */
    targetSlot: number;
    bundle: OtaBundleInfo;
    board: OtaBoardInfo;
    progress: OtaProgressInfo;
}

/** What the panel shows before the hub has said anything. */
export const IDLE_OTA: OtaState = {
    phase: "idle",
    verdict: "none",
    verdictText: "",
    lastError: "",
    autoConfirm: true,
    confirmReady: false,
    targetSlot: -1,
    bundle: {
        loaded: false,
        path: "",
        name: "",
        mcuId: 0,
        version: "0.0.0",
        gitHash: "",
        protocolVersion: 0,
        images: [],
    },
    board: {
        seen: false,
        mcuId: 0,
        runningSlot: 0,
        slotState: [],
        slotStateNames: [],
        updaterBusy: false,
        version: "0.0.0",
        gitHash: "",
        slotSize: 0,
        maxChunkData: 0,
    },
    progress: { sentBytes: 0, ackedBytes: 0, totalBytes: 0, retries: 0, percent: 0 },
};

const PHASES = new Set<string>([
    "idle",
    "query",
    "erasing",
    "transfer",
    "verifying",
    "rebooting",
    "waitingBoard",
    "testing",
    "confirmed",
    "rolledBack",
    "reverting",
    "failed",
]);

const VERDICTS = new Set<string>(["none", "confirmed", "rolledBack", "reverted", "failed"]);

/** Phases in which a session occupies the board link. */
const RUNNING = new Set<OtaPhase>([
    "query",
    "erasing",
    "transfer",
    "verifying",
    "rebooting",
    "waitingBoard",
    "testing",
    "reverting",
]);

const PHASE_WORDS: Record<OtaPhase, string> = {
    idle: "idle",
    query: "asking the board what it runs",
    erasing: "erasing the target slot",
    transfer: "sending the image",
    verifying: "the board is checking the image",
    rebooting: "rebooting into the new image",
    waitingBoard: "waiting for the board to come back",
    testing: "the new image runs on trial",
    confirmed: "confirmed",
    rolledBack: "rolled back",
    reverting: "activating the other slot",
    failed: "failed",
};

function readString(source: Record<string, unknown>, key: string, fallback = ""): string {
    const value = source[key];
    return typeof value === "string" ? value : fallback;
}

function readNumber(source: Record<string, unknown>, key: string, fallback = 0): number {
    const value = Number(source[key]);
    return Number.isFinite(value) ? value : fallback;
}

function readBool(source: Record<string, unknown>, key: string, fallback = false): boolean {
    const value = source[key];
    return typeof value === "boolean" ? value : fallback;
}

function readObject(source: Record<string, unknown>, key: string): Record<string, unknown> {
    const value = source[key];
    return typeof value === "object" && value !== null ? (value as Record<string, unknown>) : {};
}

function readNumbers(source: Record<string, unknown>, key: string): number[] {
    const value = source[key];
    return Array.isArray(value) ? value.map((entry) => Number(entry)) : [];
}

function readStrings(source: Record<string, unknown>, key: string): string[] {
    const value = source[key];
    return Array.isArray(value) ? value.map((entry) => String(entry)) : [];
}

/**
 * Decodes one `ota` message. A field the hub did not send keeps the idle
 * default: an older page must survive a newer hub and the other way round.
 */
export function readOtaState(message: Record<string, unknown>): OtaState {
    const phase = readString(message, "phase", IDLE_OTA.phase);
    const verdict = readString(message, "verdict", IDLE_OTA.verdict);
    const bundle = readObject(message, "bundle");
    const board = readObject(message, "board");
    const progress = readObject(message, "progress");
    const images = bundle["images"];
    return {
        phase: (PHASES.has(phase) ? phase : IDLE_OTA.phase) as OtaPhase,
        verdict: (VERDICTS.has(verdict) ? verdict : IDLE_OTA.verdict) as OtaVerdict,
        verdictText: readString(message, "verdictText"),
        lastError: readString(message, "lastError"),
        autoConfirm: readBool(message, "autoConfirm", true),
        confirmReady: readBool(message, "confirmReady"),
        targetSlot: readNumber(message, "targetSlot", -1),
        bundle: {
            loaded: readBool(bundle, "loaded"),
            path: readString(bundle, "path"),
            name: readString(bundle, "name"),
            mcuId: readNumber(bundle, "mcuId"),
            version: readString(bundle, "version", IDLE_OTA.bundle.version),
            gitHash: readString(bundle, "gitHash"),
            protocolVersion: readNumber(bundle, "protocolVersion"),
            images: Array.isArray(images)
                ? images.map((entry) => {
                      const image = entry as Record<string, unknown>;
                      return {
                          slot: readNumber(image, "slot"),
                          size: readNumber(image, "size"),
                          crc32: readNumber(image, "crc32"),
                      };
                  })
                : [],
        },
        board: {
            seen: readBool(board, "seen"),
            mcuId: readNumber(board, "mcuId"),
            runningSlot: readNumber(board, "runningSlot"),
            slotState: readNumbers(board, "slotState"),
            slotStateNames: readStrings(board, "slotStateNames"),
            updaterBusy: readBool(board, "updaterBusy"),
            version: readString(board, "version", IDLE_OTA.board.version),
            gitHash: readString(board, "gitHash"),
            slotSize: readNumber(board, "slotSize"),
            maxChunkData: readNumber(board, "maxChunkData"),
        },
        progress: {
            sentBytes: readNumber(progress, "sentBytes"),
            ackedBytes: readNumber(progress, "ackedBytes"),
            totalBytes: readNumber(progress, "totalBytes"),
            retries: readNumber(progress, "retries"),
            percent: readNumber(progress, "percent"),
        },
    };
}

/** True while a session occupies the board link. */
export function otaRunning(state: OtaState): boolean {
    return RUNNING.has(state.phase);
}

/** The phase in the words an operator reads. */
export function phaseLabel(phase: OtaPhase): string {
    return PHASE_WORDS[phase] ?? phase;
}

/** How a phase should be colored: good, bad, busy or nothing. */
export function phaseTone(state: OtaState): "good" | "bad" | "warn" | "" {
    if (state.phase === "confirmed") {
        return "good";
    }
    if (state.phase === "failed" || state.phase === "rolledBack") {
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

/** One identity line: "v1.2.3 (deadbeef)", the hash left out when unknown. */
export function identityText(version: string, gitHash: string): string {
    return gitHash === "" ? `v${version}` : `v${version} (${gitHash})`;
}

/** The slot line of the board: which one runs, and what each one holds. */
export function slotsText(board: OtaBoardInfo): string {
    if (!board.seen) {
        return "";
    }
    const states = board.slotStateNames.map(
        (name, slot) => `${slotLetter(slot)} ${name}${slot === board.runningSlot ? " (running)" : ""}`
    );
    return states.join(", ");
}

/** The bar caption: what the board has written, out of what it was promised. */
export function progressText(state: OtaState): string {
    if (state.progress.totalBytes === 0) {
        return "";
    }
    const percent = Math.round(state.progress.percent);
    const retries = state.progress.retries;
    const resent = retries === 0 ? "" : `, ${retries} resend${retries === 1 ? "" : "s"}`;
    return `${state.progress.ackedBytes} / ${state.progress.totalBytes} bytes (${percent}%${resent})`;
}

/**
 * The verdict in plain words. The hub writes the sentence, so both a page and
 * a script read the same one; this only fills in for a hub that sent none.
 */
export function verdictText(state: OtaState): string {
    if (state.verdictText !== "") {
        return state.verdictText;
    }
    if (state.lastError !== "") {
        return state.lastError;
    }
    return "";
}

/** Which of the panel's controls make sense in the current state. */
export interface OtaActions {
    /** Start the update. */
    start: boolean;
    /** Drop the running update. */
    abort: boolean;
    /** Confirm the trial image by hand. */
    confirm: boolean;
    /** Ask the board to go back to its other slot. */
    revert: boolean;
    /** Let the operator type a bundle path. */
    editBundle: boolean;
    /** Change the auto-confirm policy. */
    editPolicy: boolean;
}

/**
 * A control is offered only when it would do something. `boardLinked` is the
 * hub's serial link: with no link there is nothing to update, and offering
 * the buttons anyway would only produce refusals.
 */
export function otaActions(state: OtaState, boardLinked: boolean): OtaActions {
    const running = otaRunning(state);
    return {
        start: boardLinked && !running,
        abort: running,
        // Automatic mode sends the confirmation itself; a button next to it
        // would race the hub for the same gesture.
        confirm: boardLinked && state.phase === "testing" && !state.autoConfirm,
        revert: boardLinked && (!running || state.phase === "testing"),
        editBundle: !running,
        editPolicy: !running || state.phase === "testing",
    };
}
