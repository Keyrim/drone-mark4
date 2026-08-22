/**
 * The RC state a drone widget streams, and the few pure rules around it.
 *
 * There is no engage ritual and no keyboard layer: the switches and the
 * throttle slider ARE the transmitter. The widget starts streaming at
 * TICK_MS on the first interaction and never stops while the page is
 * visible; the safety net is the drone's own RC timeout, which turns any
 * silence - a closed tab, a frozen browser, a dead link - into a cut.
 *
 * The rules that must never be weakened:
 *
 * - The safe state is kill engaged, disarmed, stick down. Every stream
 *   starts from it, and hiding the page returns to it: a pilot who cannot
 *   see the drone is not piloting it.
 * - The throttle is clamped to [0, 1] whatever the input element reports.
 */

/** Stream period once a widget transmits [ms]. */
export const TICK_MS = 100;

export const MODE_MANUAL = 0;
export const MODE_ALTITUDE_AUTO = 1;

export interface RcState {
    readonly kill: boolean;
    readonly arm: boolean;
    readonly mode: number;
    /** Normalized [0, 1] */
    readonly throttle: number;
}

/** The safe state: cut, disarmed, stick down. */
export const SAFE_RC: RcState = {
    kill: true,
    arm: false,
    mode: MODE_MANUAL,
    throttle: 0,
};

export function clamp01(value: number): number {
    return value < 0 ? 0 : value > 1 ? 1 : Number.isNaN(value) ? 0 : value;
}

/** The rc message body for one state, exactly what the hub expects. */
export function rcPayload(state: RcState, target: string): Record<string, unknown> {
    return {
        type: "rc",
        target,
        kill: state.kill ? 1 : 0,
        arm: state.arm ? 1 : 0,
        mode: state.mode,
        throttle: clamp01(state.throttle),
    };
}
