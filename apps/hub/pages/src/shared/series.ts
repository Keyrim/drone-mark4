/**
 * Catalog of the series a page can plot, and how one telemetry row is turned
 * into their values.
 *
 * The estimated state travels in the telemetry message, the exact state in
 * the simRaw message a simulator emits beside it. They are two independent
 * streams with their own timestamps, but a lane is one x axis, so the exact
 * state is resampled onto the telemetry timestamps: a simRaw message only
 * latches its values, and the latch is read when the next telemetry row
 * lands. This is the causal form of the hub's alignment rule; the hub's
 * offline compare stays the authority on the aggregate score.
 */

import { asQuat, errorAngleDeg, eulerDeg, type Quat } from "./quat";

/**
 * Categorical series palette (validated: colour-vision-deficiency
 * separation, chroma, lightness band and contrast against a dark surface).
 */
export const PALETTE = [
    "#3987e5",
    "#199e70",
    "#c98500",
    "#008300",
    "#9085e9",
    "#e66767",
    "#d55181",
    "#d95926",
];

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
    /** Value for one aligned row, null when the source is absent */
    readonly value: (row: TelemetryRow, exact: ExactState | null) => number | null;
}

/** Telemetry message fields this module reads. */
export interface TelemetryRow {
    timestampUs: number;
    gyroRadS: number[];
    attitudeQuat: number[];
    motor: number[];
    altitudeM: number;
    verticalVelocityMps: number;
    flightPhase: number;
    throwState: number;
}

/** Latched exact simulator state. */
export interface ExactState {
    timestampUs: number;
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
];

const AXES = ["x", "y", "z"];
const EULER = ["roll", "pitch", "yaw"];
const DASH = [6, 4];

function estimatedEuler(row: TelemetryRow): number[] | null {
    const q = asQuat(row.attitudeQuat);
    return q === null ? null : eulerDeg(q);
}

/** Every series a live telemetry (plus simRaw) stream can feed. */
export const LIVE_SERIES: SeriesDef[] = [
    ...AXES.map((axis, i) => ({
        key: `gyro.${axis}`,
        label: `gyro ${axis}`,
        unit: "rad/s",
        color: PALETTE[i],
        value: (row: TelemetryRow) => row.gyroRadS[i] ?? null,
    })),
    ...EULER.map((name, i) => ({
        key: `euler.est.${name}`,
        label: `${name} est`,
        unit: "deg",
        color: PALETTE[i],
        value: (row: TelemetryRow) => estimatedEuler(row)?.[i] ?? null,
    })),
    ...EULER.map((name, i) => ({
        key: `euler.exact.${name}`,
        label: `${name} exact`,
        unit: "deg",
        color: PALETTE[i],
        dash: DASH,
        value: (_row: TelemetryRow, exact: ExactState | null) =>
            exact === null ? null : eulerDeg(exact.attitudeQuat)[i] ?? null,
    })),
    {
        key: "attitude.error",
        label: "attitude error",
        unit: "deg",
        color: PALETTE[7],
        value: (row, exact) => {
            const q = asQuat(row.attitudeQuat);
            return q === null || exact === null ? null : errorAngleDeg(q, exact.attitudeQuat);
        },
    },
    {
        key: "alt.est",
        label: "altitude est",
        unit: "m",
        color: PALETTE[0],
        value: (row) => row.altitudeM,
    },
    {
        key: "alt.exact",
        label: "altitude exact",
        unit: "m",
        color: PALETTE[0],
        dash: DASH,
        value: (_row, exact) => exact?.positionM[2] ?? null,
    },
    {
        key: "vz.est",
        label: "vz est",
        unit: "m/s",
        color: PALETTE[1],
        value: (row) => row.verticalVelocityMps,
    },
    {
        key: "vz.exact",
        label: "vz exact",
        unit: "m/s",
        color: PALETTE[1],
        dash: DASH,
        value: (_row, exact) => exact?.velocityMps[2] ?? null,
    },
    ...[0, 1, 2, 3].map((i) => ({
        key: `motor.${i}`,
        label: `motor ${i}`,
        unit: "",
        color: PALETTE[i],
        value: (row: TelemetryRow) => row.motor[i] ?? null,
    })),
    {
        key: "flightPhase",
        label: "flight phase",
        unit: "",
        color: PALETTE[4],
        value: (row) => row.flightPhase,
    },
    {
        key: "throwState",
        label: "throw state",
        unit: "",
        color: PALETTE[5],
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
    { title: "vertical", keys: ["alt.est", "alt.exact", "vz.est", "vz.exact"] },
];

/** One aligned sample: a timestamp and the value of every catalog series. */
export interface SampledRow {
    timestampUs: number;
    values: Map<string, number | null>;
}

/**
 * Turns the two live streams into aligned rows. simRaw messages never push a
 * row of their own: they latch, and the latch is sampled by the next
 * telemetry row.
 */
export class LiveSampler {
    private exact: ExactState | null = null;

    latchSimRaw(message: Record<string, unknown>): void {
        const quat = asQuat(message["attitudeQuat"]);
        if (quat === null) {
            return;
        }
        this.exact = {
            timestampUs: Number(message["timestampUs"]),
            attitudeQuat: quat,
            positionM: (message["positionM"] as number[]) ?? [],
            velocityMps: (message["velocityMps"] as number[]) ?? [],
        };
    }

    /** Gap between the latched exact state and this row [us], null if none. */
    latchGapUs(timestampUs: number): number | null {
        return this.exact === null ? null : timestampUs - this.exact.timestampUs;
    }

    sample(message: Record<string, unknown>): SampledRow {
        const row: TelemetryRow = {
            timestampUs: Number(message["timestampUs"]),
            gyroRadS: (message["gyroRadS"] as number[]) ?? [],
            attitudeQuat: (message["attitudeQuat"] as number[]) ?? [],
            motor: (message["motor"] as number[]) ?? [],
            altitudeM: Number(message["altitudeM"]),
            verticalVelocityMps: Number(message["verticalVelocityMps"]),
            flightPhase: Number(message["flightPhase"]),
            throwState: Number(message["throwState"]),
        };
        const values = new Map<string, number | null>();
        for (const def of LIVE_SERIES) {
            values.set(def.key, def.value(row, this.exact));
        }
        return { timestampUs: row.timestampUs, values };
    }
}
