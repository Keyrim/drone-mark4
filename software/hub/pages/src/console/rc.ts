/**
 * The RC state a drone widget streams, and the few pure rules around it.
 *
 * There is no engage ritual and no keyboard layer: the switches, the mode
 * selector and the throttle slider ARE the transmitter. The widget starts
 * streaming at TICK_MS on the first interaction and never stops while the
 * page is visible; the safety net is the drone's own RC timeout, which
 * turns any silence - a closed tab, a frozen browser, a dead link - into a
 * cut.
 *
 * The console has no sticks: it streams released ones (zero on the three
 * axes). That is why its natural direct-thrust mode is LEVEL, where a
 * released stick means "level", and not MANUAL, where it means "hold
 * whatever rate the drone has" and nothing levels it. The gamepad path
 * (the phone) is what MANUAL is for.
 *
 * The rules that must never be weakened:
 *
 * - The safe state is kill engaged, disarmed, stick down, sticks released.
 *   Every stream starts from it, and hiding the page returns to it: a pilot
 *   who cannot see the drone is not piloting it.
 * - The throttle is clamped to [0, 1] and each stick to [-1, 1] whatever
 *   the input element reports.
 */

import { create } from "@bufbuild/protobuf";

import { type Envelope, EnvelopeSchema, RcMode } from "../gen/mark4_pb";

/**
 * Stream period once a widget transmits [ms]: 20 Hz, four messages inside
 * the drone's 200 ms fail-safe window.
 */
export const TICK_MS = 50;

export const MODE_MANUAL = RcMode.RC_MANUAL;
export const MODE_ALTITUDE_AUTO = RcMode.RC_ALTITUDE_AUTO;
export const MODE_LEVEL = RcMode.RC_LEVEL;

/** The modes in the order a selector lists them, with the word for each. */
export const MODE_OPTIONS: ReadonlyArray<{ readonly mode: RcMode; readonly label: string }> = [
    { mode: MODE_LEVEL, label: "level" },
    { mode: MODE_MANUAL, label: "manual" },
    { mode: MODE_ALTITUDE_AUTO, label: "altitude auto" },
];

export interface RcState {
    readonly kill: boolean;
    readonly arm: boolean;
    readonly mode: RcMode;
    /** Normalized [0, 1] */
    readonly throttle: number;
    /** Normalized [-1, 1], positive rolls right */
    readonly roll: number;
    /** Normalized [-1, 1], positive noses down */
    readonly pitch: number;
    /** Normalized [-1, 1], positive turns clockwise */
    readonly yaw: number;
}

/** The safe state: cut, disarmed, stick down, sticks released. */
export const SAFE_RC: RcState = {
    kill: true,
    arm: false,
    mode: MODE_MANUAL,
    throttle: 0,
    roll: 0,
    pitch: 0,
    yaw: 0,
};

export function clamp01(value: number): number {
    return value < 0 ? 0 : value > 1 ? 1 : Number.isNaN(value) ? 0 : value;
}

/** A stick axis kept inside [-1, 1], NaN read as released. */
export function clampAxis(value: number): number {
    return value < -1 ? -1 : value > 1 ? 1 : Number.isNaN(value) ? 0 : value;
}

/** The Rc envelope for one state, exactly what the flight process reads. */
export function rcEnvelope(state: RcState): Envelope {
    return create(EnvelopeSchema, {
        body: {
            case: "rc",
            value: {
                kill: state.kill,
                arm: state.arm,
                mode: state.mode,
                throttle: clamp01(state.throttle),
                roll: clampAxis(state.roll),
                pitch: clampAxis(state.pitch),
                yaw: clampAxis(state.yaw),
            },
        },
    });
}
