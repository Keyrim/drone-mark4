/**
 * Catalog of the series a page can plot, and how one Telemetry envelope is
 * turned into their values.
 *
 * The estimated state is the telemetry itself; the exact state, when the
 * sender has a plant, rides inside it as `truth`, sampled at the same
 * instant. One message, one row: the two never need aligning.
 */

import { type Telemetry } from "../gen/mark4_pb";
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
    readonly value: (row: Telemetry, exact: ExactState | null) => number | null;
}

/** The plant's exact state, read out of Telemetry.truth. */
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

const AXES = ["x", "y", "z"];
const EULER = ["roll", "pitch", "yaw"];
const DASH = [6, 4];

function estimatedEuler(row: Telemetry): number[] | null {
    const q = asQuat(row.attitudeQuat);
    return q === null ? null : eulerDeg(q);
}

/** Every series a live telemetry stream can feed. */
export const LIVE_SERIES: SeriesDef[] = [
    ...AXES.map((axis, i) => ({
        key: `gyro.${axis}`,
        label: `gyro ${axis}`,
        unit: "rad/s",
        color: PALETTE[i] as string,
        value: (row: Telemetry) => row.gyroRadS[i] ?? null,
    })),
    ...EULER.map((name, i) => ({
        key: `euler.est.${name}`,
        label: `${name} est`,
        unit: "deg",
        color: PALETTE[i] as string,
        value: (row: Telemetry) => estimatedEuler(row)?.[i] ?? null,
    })),
    ...EULER.map((name, i) => ({
        key: `euler.exact.${name}`,
        label: `${name} exact`,
        unit: "deg",
        color: PALETTE[i] as string,
        dash: DASH,
        value: (_row: Telemetry, exact: ExactState | null) =>
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
        key: "alt.est",
        label: "altitude est",
        unit: "m",
        color: PALETTE[0] as string,
        value: (row) => row.altitudeM,
    },
    {
        // The raw pressure channel the estimate is corrected toward, on the
        // same lane and the same unit as alt.est: the gap between the two
        // curves IS what the baro contributes.
        key: "alt.baro",
        label: "altitude baro",
        unit: "m",
        color: PALETTE[2] as string,
        value: (row) => row.baroAltitudeM,
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
        key: "vz.est",
        label: "vz est",
        unit: "m/s",
        color: PALETTE[1] as string,
        value: (row) => row.verticalVelocityMps,
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
        value: (row: Telemetry) => row.motor[i] ?? null,
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

/** The four plots the ground station has always drawn, in that order. */
export const DEFAULT_LANES: { title: string; keys: string[] }[] = [
    { title: "gyro", keys: ["gyro.x", "gyro.y", "gyro.z"] },
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
    { title: "vertical", keys: ["alt.est", "alt.baro", "alt.exact", "vz.est", "vz.exact"] },
];

/** One sample: a timestamp and the value of every catalog series. */
export interface SampledRow {
    timestampUs: number;
    values: Map<string, number | null>;
}

/** The exact state a telemetry message carries, null when it has none. */
export function exactStateOf(row: Telemetry): ExactState | null {
    const truth = row.truth;
    if (truth === undefined) {
        return null;
    }
    const quat = asQuat(truth.attitudeQuat);
    return quat === null
        ? null
        : { attitudeQuat: quat, positionM: truth.positionM, velocityMps: truth.velocityMps };
}

/** Turns one telemetry message into the row of every catalog series. */
export function sampleTelemetry(row: Telemetry): SampledRow {
    const exact = exactStateOf(row);
    const values = new Map<string, number | null>();
    for (const def of LIVE_SERIES) {
        values.set(def.key, def.value(row, exact));
    }
    return { timestampUs: Number(row.timestampUs), values };
}
