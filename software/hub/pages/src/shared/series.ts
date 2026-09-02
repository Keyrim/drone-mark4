/**
 * Catalog of the series a page can plot, and how one Status envelope is
 * turned into their values.
 *
 * The estimated attitude is Status itself; the exact state, when the sender
 * has a plant, rides inside it as `truth`, sampled at the same instant. One
 * message, one row: the two never need aligning.
 *
 * Everything Status stopped carrying (the gyro, the altitudes, the vertical
 * velocity, the release and the apex) is a telemetry measure now, streamed
 * on demand rather than pushed at 50 Hz, and is not in this catalog.
 */

import { type Status } from "../gen/mark4_pb";
import { PALETTE } from "./nodes";
import { asQuat, errorAngleDeg, eulerDeg, type Quat } from "./quat";

export { PALETTE };

/** One plottable quantity: stable key, how it looks, how it reads. */
export interface SeriesDef {
    /** Stable identity, stored in the view configs */
    readonly key: string;
    readonly label: string;
    /** Axis unit, empty when unitless */
    readonly unit: string;
    readonly color: string;
    /** Dash pattern, set on the exact-state twins so a lane stays readable */
    readonly dash?: number[];
    /** Value for one row, null when the source is absent */
    readonly value: (row: Status, exact: ExactState | null) => number | null;
}

/** The plant's exact state, read out of Status.truth. */
export interface ExactState {
    attitudeQuat: Quat;
    positionM: number[];
    velocityMps: number[];
}

export const FLIGHT_PHASE_NAMES = [
    "idle",
    "altitude auto",
    "armed",
    "ballistic",
    "recovery",
    "hover",
    "cutoff",
    "manual",
    "fault",
];

const EULER = ["roll", "pitch", "yaw"];
const DASH = [6, 4];

function estimatedEuler(row: Status): number[] | null {
    const q = asQuat(row.attitudeQuat);
    return q === null ? null : eulerDeg(q);
}

/** Every series a live Status stream can feed. */
export const LIVE_SERIES: SeriesDef[] = [
    ...EULER.map((name, i) => ({
        key: `euler.est.${name}`,
        label: `${name} est`,
        unit: "deg",
        color: PALETTE[i] as string,
        value: (row: Status) => estimatedEuler(row)?.[i] ?? null,
    })),
    ...EULER.map((name, i) => ({
        key: `euler.exact.${name}`,
        label: `${name} exact`,
        unit: "deg",
        color: PALETTE[i] as string,
        dash: DASH,
        value: (_row: Status, exact: ExactState | null) =>
            exact === null ? null : (eulerDeg(exact.attitudeQuat)[i] ?? null),
    })),
    {
        key: "attitude.error",
        label: "attitude error",
        unit: "deg",
        color: PALETTE[7] as string,
        value: (row, exact) => {
            const q = asQuat(row.attitudeQuat);
            return q === null || exact === null ? null : errorAngleDeg(q, exact.attitudeQuat);
        },
    },
    {
        key: "alt.exact",
        label: "altitude exact",
        unit: "m",
        color: PALETTE[0] as string,
        dash: DASH,
        value: (_row, exact) => exact?.positionM[2] ?? null,
    },
    {
        key: "vz.exact",
        label: "vz exact",
        unit: "m/s",
        color: PALETTE[1] as string,
        dash: DASH,
        value: (_row, exact) => exact?.velocityMps[2] ?? null,
    },
    ...[0, 1, 2, 3].map((i) => ({
        key: `motor.${i}`,
        label: `motor ${i}`,
        unit: "",
        color: PALETTE[i] as string,
        value: (row: Status) => row.motor[i] ?? null,
    })),
    {
        key: "flightPhase",
        label: "flight phase",
        unit: "",
        color: PALETTE[4] as string,
        value: (row) => row.flightPhase,
    },
    {
        key: "throwState",
        label: "throw state",
        unit: "",
        color: PALETTE[5] as string,
        value: (row) => row.throwState,
    },
];

export function seriesByKey(key: string): SeriesDef | undefined {
    return LIVE_SERIES.find((def) => def.key === key);
}

/** The lanes the page opens on, in that order. */
export const DEFAULT_LANES: { title: string; keys: string[] }[] = [
    {
        title: "attitude",
        keys: [
            "euler.est.roll",
            "euler.est.pitch",
            "euler.est.yaw",
            "euler.exact.roll",
            "euler.exact.pitch",
            "euler.exact.yaw",
        ],
    },
    { title: "attitude error", keys: ["attitude.error"] },
    { title: "vertical", keys: ["alt.exact", "vz.exact"] },
];

/** One sample: a timestamp and the value of every catalog series. */
export interface SampledRow {
    timestampUs: number;
    values: Map<string, number | null>;
}

/** The exact state a Status message carries, null when it has none. */
export function exactStateOf(row: Status): ExactState | null {
    const truth = row.truth;
    if (truth === undefined) {
        return null;
    }
    const quat = asQuat(truth.attitudeQuat);
    return quat === null
        ? null
        : { attitudeQuat: quat, positionM: truth.positionM, velocityMps: truth.velocityMps };
}

/** Turns one Status message into the row of every catalog series. */
export function sampleStatus(row: Status): SampledRow {
    const exact = exactStateOf(row);
    const values = new Map<string, number | null>();
    for (const def of LIVE_SERIES) {
        values.set(def.key, def.value(row, exact));
    }
    return { timestampUs: Number(row.timestampUs), values };
}
