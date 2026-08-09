/**
 * The RC state a browser can pilot with, and the pure reducer that moves it.
 *
 * There is exactly ONE state, mutated by the buttons and by the keyboard
 * alike, and it is streamed only while the pilot toggle is engaged. Nothing
 * here talks to the DOM or to a socket: the events come in, the next state
 * comes out, and the page is the one that sends it. That is what makes the
 * safety rules testable.
 *
 * The rules that must never be weakened:
 *
 * - Space is a panic kill and can never un-kill. The kill toggle is the only
 *   way back, and only while engaged.
 * - Disengaging resets the whole state to the safe one: kill engaged, motors
 *   disarmed, stick at zero. The page then sends that twice and stops
 *   streaming, and the silence itself is the fail-safe: the drone cuts on its
 *   own RC timeout, so a browser that freezes or a tab that closes is already
 *   handled.
 * - The throttle ramp is applied at the stream tick, not at the key event, so
 *   a stuck key ramps at a known rate and a dropped keyup cannot run away
 *   faster than the deadman allows.
 */

/** Throttle ramp of a held arrow key [fraction of full scale per second]. */
export const RAMP_PER_S = 0.4;

/** Same, with Shift held: the fine ramp. */
export const RAMP_FINE_PER_S = 0.08;

/** Stream period while engaged [ms]. */
export const TICK_MS = 100;

/**
 * A tick arriving later than this disengages. A late tick means the page is
 * not being scheduled (background tab, blocked main thread), and a pilot who
 * is not being scheduled is not piloting.
 */
export const LATE_TICK_MS = 300;

/** No key and no button for this long disengages [ms]. */
export const DEADMAN_MS = 60000;

/** Throttle the centering key sets: altitude-auto arms on a centered stick. */
export const CENTER_THROTTLE = 0.5;

export const MODE_MANUAL = 0;
export const MODE_ALTITUDE_AUTO = 1;

/** Keys whose held state the ramp reads. */
export type HeldKey = "up" | "down" | "fine";

export interface RcState {
    /** True while the state is being streamed to a flight process */
    readonly engaged: boolean;
    readonly kill: boolean;
    readonly arm: boolean;
    readonly mode: number;
    /** Normalized [0, 1] */
    readonly throttle: number;
    readonly held: ReadonlySet<HeldKey>;
    /** Last key or button, the deadman reads it [ms] */
    readonly lastActivityMs: number;
    /** Last tick applied, the ramp and the late check read it [ms] */
    readonly lastTickMs: number;
    /** Why the last disengage happened, empty while nothing happened */
    readonly reason: string;
}

/** The safe state: cut, disarmed, stick down, streaming nothing. */
export const SAFE_RC: RcState = {
    engaged: false,
    kill: true,
    arm: false,
    mode: MODE_MANUAL,
    throttle: 0,
    held: new Set<HeldKey>(),
    lastActivityMs: 0,
    lastTickMs: 0,
    reason: "",
};

export type RcEvent =
    | { readonly type: "engage"; readonly atMs: number }
    | { readonly type: "disengage"; readonly reason: string }
    | { readonly type: "panic" }
    | { readonly type: "toggleKill"; readonly atMs: number }
    | { readonly type: "toggleArm"; readonly atMs: number }
    | { readonly type: "toggleMode"; readonly atMs: number }
    | { readonly type: "setThrottle"; readonly value: number; readonly atMs: number }
    | { readonly type: "hold"; readonly key: HeldKey; readonly down: boolean; readonly atMs: number }
    | { readonly type: "tick"; readonly atMs: number };

function clamp01(value: number): number {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

/** Back to safe, keeping the mode: it is a preference, not a command. */
function disengaged(state: RcState, reason: string): RcState {
    return { ...SAFE_RC, mode: state.mode, held: new Set<HeldKey>(), reason };
}

/**
 * Next state for one event. Pure: same state and same event, same answer.
 *
 * Everything but a panic, a disengage and a tick is ignored while
 * disengaged - the buttons are dead until the pilot toggle is on, and a
 * one-shot arm would die at the drone's RC timeout anyway.
 */
export function rcReduce(state: RcState, event: RcEvent): RcState {
    switch (event.type) {
        case "engage":
            return {
                ...SAFE_RC,
                mode: state.mode,
                engaged: true,
                held: new Set<HeldKey>(),
                lastActivityMs: event.atMs,
                lastTickMs: event.atMs,
            };
        case "disengage":
            return disengaged(state, event.reason);
        case "panic":
            // Never an un-kill, engaged or not, whatever else is going on
            return { ...state, kill: true, throttle: 0 };
        case "tick": {
            if (!state.engaged) {
                return state;
            }
            if (event.atMs - state.lastTickMs > LATE_TICK_MS) {
                return disengaged(state, "the stream fell behind");
            }
            if (event.atMs - state.lastActivityMs >= DEADMAN_MS) {
                return disengaged(state, "deadman: no input for 60 s");
            }
            const seconds = (event.atMs - state.lastTickMs) / 1000;
            const rate = state.held.has("fine") ? RAMP_FINE_PER_S : RAMP_PER_S;
            const direction = (state.held.has("up") ? 1 : 0) - (state.held.has("down") ? 1 : 0);
            return {
                ...state,
                throttle: clamp01(state.throttle + direction * rate * seconds),
                lastTickMs: event.atMs,
            };
        }
        default:
            break;
    }
    if (!state.engaged) {
        return state;
    }
    switch (event.type) {
        case "toggleKill":
            return { ...state, kill: !state.kill, lastActivityMs: event.atMs };
        case "toggleArm":
            return { ...state, arm: !state.arm, lastActivityMs: event.atMs };
        case "toggleMode":
            return {
                ...state,
                mode: state.mode === MODE_MANUAL ? MODE_ALTITUDE_AUTO : MODE_MANUAL,
                lastActivityMs: event.atMs,
            };
        case "setThrottle":
            return { ...state, throttle: clamp01(event.value), lastActivityMs: event.atMs };
        case "hold": {
            const held = new Set(state.held);
            if (event.down) {
                held.add(event.key);
            } else {
                held.delete(event.key);
            }
            return { ...state, held, lastActivityMs: event.atMs };
        }
        default:
            return state;
    }
}

/**
 * The event one key press or release stands for, null when the key means
 * nothing here. An auto-repeat press is nothing: holding an arrow must ramp
 * once per tick, not once per repeat, and holding a toggle must not flap it.
 */
export function keyEvent(
    code: string,
    shiftKey: boolean,
    repeat: boolean,
    down: boolean,
    atMs: number
): RcEvent | null {
    if (down && repeat) {
        return null;
    }
    switch (code) {
        case "ArrowUp":
            return { type: "hold", key: "up", down, atMs };
        case "ArrowDown":
            return { type: "hold", key: "down", down, atMs };
        case "ShiftLeft":
        case "ShiftRight":
            return { type: "hold", key: "fine", down, atMs };
        default:
            break;
    }
    if (!down) {
        return null;
    }
    switch (code) {
        case "Space":
            return { type: "panic" };
        case "Escape":
            return { type: "disengage", reason: "escape" };
        case "KeyK":
            return { type: "toggleKill", atMs };
        case "KeyA":
            return { type: "toggleArm", atMs };
        case "KeyM":
            return { type: "toggleMode", atMs };
        case "KeyC":
            return { type: "setThrottle", value: shiftKey ? 0 : CENTER_THROTTLE, atMs };
        case "Digit0":
        case "Numpad0":
        case "Home":
            return { type: "setThrottle", value: 0, atMs };
        default:
            return null;
    }
}

/** The rc message body for one state, exactly what the hub expects. */
export function rcPayload(state: RcState, target: string): Record<string, unknown> {
    return {
        type: "rc",
        target,
        kill: state.kill ? 1 : 0,
        arm: state.arm ? 1 : 0,
        mode: state.mode,
        throttle: state.throttle,
    };
}
