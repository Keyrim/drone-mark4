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

import { create } from "@bufbuild/protobuf";

import { type Envelope, EnvelopeSchema, RcMode } from "../gen/mark4_pb";

/** Stream period once a widget transmits [ms]. */
export const TICK_MS = 100;

export const MODE_MANUAL = RcMode.RC_MANUAL;
export const MODE_ALTITUDE_AUTO = RcMode.RC_ALTITUDE_AUTO;

export interface RcState {
    readonly kill: boolean;
    readonly arm: boolean;
    readonly mode: RcMode;
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
            },
        },
    });
}
