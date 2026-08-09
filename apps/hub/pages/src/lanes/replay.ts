/**
 * Filling the lanes from a recording the hub holds.
 *
 * A streams recording carries the same two streams as the live link, so it
 * is replayed through the same rule: a simRaw row only latches, and the
 * latch is sampled when the next telemetry row lands. A blackbox recording
 * carries raw sensors and actuator commands instead, with no estimate and no
 * exact state, so its series are its columns.
 */

import { columnIndex, type Table } from "../shared/api";
import {
    LIVE_SERIES,
    PALETTE,
    type ExactState,
    type SeriesDef,
    type TelemetryRow,
} from "../shared/series";
import { asQuat } from "../shared/quat";
import { SeriesBuffer, type LaneConfig } from "./model";

const US_PER_S = 1e6;

/** Lanes and buffers ready to hand to a LanesView. */
export interface Filled {
    buffers: Map<string, SeriesBuffer>;
    lanes: LaneConfig[];
    durationS: number;
}

function cell(row: number[], index: Map<string, number>, name: string): number {
    const at = index.get(name);
    return at === undefined ? Number.NaN : (row[at] ?? Number.NaN);
}

function triple(row: number[], index: Map<string, number>, names: string[]): number[] {
    return names.map((name) => cell(row, index, name));
}

/**
 * Rebuild a telemetry row from a CSV-shaped table row. Columns the recording
 * does not carry come back as NaN and their series stay empty.
 */
function telemetryRow(row: number[], index: Map<string, number>): TelemetryRow {
    return {
        timestampUs: cell(row, index, "timestamp_us"),
        gyroRadS: triple(row, index, ["gyro_x", "gyro_y", "gyro_z"]),
        attitudeQuat: triple(row, index, ["quat_w", "quat_x", "quat_y", "quat_z"]),
        motor: triple(row, index, ["motor_0", "motor_1", "motor_2", "motor_3"]),
        altitudeM: cell(row, index, "altitude_m"),
        verticalVelocityMps: cell(row, index, "vz_mps"),
        flightPhase: cell(row, index, "flight_phase"),
        throwState: cell(row, index, "throw_state"),
    };
}

function exactState(row: number[], index: Map<string, number>): ExactState | null {
    const quat = asQuat(triple(row, index, ["quat_w", "quat_x", "quat_y", "quat_z"]));
    if (quat === null || !Number.isFinite(quat[0])) {
        return null;
    }
    return {
        timestampUs: cell(row, index, "timestamp_us"),
        attitudeQuat: quat,
        positionM: triple(row, index, ["pos_x", "pos_y", "pos_z"]),
        velocityMps: triple(row, index, ["vel_x", "vel_y", "vel_z"]),
    };
}

function finite(value: number | null): number | null {
    return value !== null && Number.isFinite(value) ? value : null;
}

/**
 * Replay a streams pair through the catalog. simRaw rows are consumed up to
 * the timestamp of the telemetry row being sampled: the same causal pairing
 * the live page does, so both modes read the same numbers.
 */
export function fillFromStreams(
    telemetry: Table,
    simRaw: Table | null,
    lanes: LaneConfig[]
): Filled {
    const buffers = new Map<string, SeriesBuffer>(
        LIVE_SERIES.map((def) => [def.key, new SeriesBuffer(def, Number.MAX_SAFE_INTEGER)])
    );
    const telemetryIndex = columnIndex(telemetry);
    const simIndex = simRaw ? columnIndex(simRaw) : null;
    const simRows = simRaw?.rows ?? [];
    let simAt = 0;
    let exact: ExactState | null = null;
    let originUs: number | null = null;
    let durationS = 0;

    for (const raw of telemetry.rows) {
        const row = telemetryRow(raw, telemetryIndex);
        if (!Number.isFinite(row.timestampUs)) {
            continue;
        }
        if (originUs === null) {
            originUs = row.timestampUs;
        }
        while (simAt < simRows.length && simIndex !== null) {
            const candidate = simRows[simAt] as number[];
            if (cell(candidate, simIndex, "timestamp_us") > row.timestampUs) {
                break;
            }
            exact = exactState(candidate, simIndex) ?? exact;
            simAt += 1;
        }
        const t = (row.timestampUs - originUs) / US_PER_S;
        durationS = t;
        for (const def of LIVE_SERIES) {
            buffers.get(def.key)?.push(t, finite(def.value(row, exact)));
        }
    }
    return { buffers, lanes, durationS };
}

/** Unit of a blackbox column, read off its name. */
function unitOf(column: string): string {
    if (column.endsWith("_rad_s")) {
        return "rad/s";
    }
    if (column.endsWith("_mps2")) {
        return "m/s^2";
    }
    if (column.endsWith("_pa")) {
        return "Pa";
    }
    return "";
}

/** Which lane a blackbox column belongs to, in display order. */
const BLACKBOX_GROUPS: { title: string; holds: (column: string) => boolean }[] = [
    { title: "gyro", holds: (c) => c.startsWith("gyro") },
    { title: "accel", holds: (c) => c.startsWith("accel") },
    { title: "baro", holds: (c) => c.startsWith("baro") },
    { title: "motors", holds: (c) => c.startsWith("motor") },
    { title: "pilot", holds: (c) => c === "throttle" || c.endsWith("_switch") },
];

/**
 * A blackbox recording has no catalog: its series are its columns, so the
 * lanes are built from the header the hub sent.
 */
export function fillFromBlackbox(table: Table): Filled {
    const buffers = new Map<string, SeriesBuffer>();
    const lanes: LaneConfig[] = [];
    const index = columnIndex(table);
    const timeAt = index.get("timestamp_us") ?? 0;

    for (const column of table.columns) {
        if (column === "timestamp_us") {
            continue;
        }
        const group = BLACKBOX_GROUPS.find((g) => g.holds(column));
        let lane = lanes.find((l) => l.title === (group?.title ?? "other"));
        if (!lane) {
            lane = { title: group?.title ?? "other", keys: [] };
            lanes.push(lane);
        }
        const def: SeriesDef = {
            key: column,
            label: column,
            unit: unitOf(column),
            color: PALETTE[lane.keys.length % PALETTE.length] as string,
            value: () => null,
        };
        lane.keys.push(column);
        buffers.set(column, new SeriesBuffer(def, Number.MAX_SAFE_INTEGER));
    }
    // Keep the groups in their declared order, leftovers last
    lanes.sort((a, b) => laneRank(a.title) - laneRank(b.title));

    let originUs: number | null = null;
    let durationS = 0;
    for (const row of table.rows) {
        const timestampUs = row[timeAt] ?? Number.NaN;
        if (!Number.isFinite(timestampUs)) {
            continue;
        }
        if (originUs === null) {
            originUs = timestampUs;
        }
        const t = (timestampUs - originUs) / US_PER_S;
        durationS = t;
        for (const [column, at] of index) {
            if (column !== "timestamp_us") {
                buffers.get(column)?.push(t, finite(row[at] ?? null));
            }
        }
    }
    return { buffers, lanes, durationS };
}

function laneRank(title: string): number {
    const at = BLACKBOX_GROUPS.findIndex((g) => g.title === title);
    return at < 0 ? BLACKBOX_GROUPS.length : at;
}
