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
    buildEpoch: number;
    gitHash: string;
    protocolVersion: number;
    images: OtaImageInfo[];
}

/** One firmware slot: its lifecycle state and the image identity in it. */
export interface OtaSlotInfo {
    state: number;
    stateName: string;
    buildEpoch: number;
    gitHash: string;
}

/** What the board last said about itself. */
export interface OtaBoardInfo {
    seen: boolean;
    mcuId: number;
    runningSlot: number;
    activeSlot: number;
    updaterBusy: boolean;
    slots: OtaSlotInfo[];
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
    targetSlot: -1,
    bundle: {
        loaded: false,
        path: "",
        name: "",
        mcuId: 0,
        buildEpoch: 0,
        gitHash: "",
        protocolVersion: 0,
        images: [],
    },
    board: {
        seen: false,
        mcuId: 0,
        runningSlot: 0,
        activeSlot: 0,
        updaterBusy: false,
        slots: [],
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
        targetSlot: readNumber(message, "targetSlot", -1),
        bundle: {
            loaded: readBool(bundle, "loaded"),
            path: readString(bundle, "path"),
            name: readString(bundle, "name"),
            mcuId: readNumber(bundle, "mcuId"),
            buildEpoch: readNumber(bundle, "buildEpoch"),
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
            activeSlot: readNumber(board, "activeSlot"),
            updaterBusy: readBool(board, "updaterBusy"),
            slots: Array.isArray(board["slots"])
                ? (board["slots"] as unknown[]).map((entry) => {
                      const slot = entry as Record<string, unknown>;
                      return {
                          state: readNumber(slot, "state", 0xff),
                          stateName: readString(slot, "stateName", "empty"),
                          buildEpoch: readNumber(slot, "buildEpoch"),
                          gitHash: readString(slot, "gitHash"),
                      };
                  })
                : [],
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

/** How a slot state should be colored on its row. */
export function slotTone(stateName: string): "good" | "bad" | "warn" | "" {
    if (stateName === "valid") {
        return "good";
    }
    if (stateName === "bad") {
        return "bad";
    }
    if (stateName === "testing" || stateName === "staged") {
        return "warn";
    }
    return "";
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
    /** Ask the board to go back to its other slot. */
    revert: boolean;
    /** Let the operator type a bundle path. */
    editBundle: boolean;
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
        revert: boardLinked && (!running || state.phase === "testing"),
        editBundle: !running,
    };
}
