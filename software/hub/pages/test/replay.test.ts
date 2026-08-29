import assert from "node:assert/strict";
import test from "node:test";

import { fillFromBlackbox, fillFromStreams } from "../src/lanes/replay";
import type { Table } from "../src/shared/api";
import { LiveSampler } from "../src/shared/series";

const TELEMETRY: Table = {
    total: 3,
    stride: 1,
    count: 3,
    columns: [
        "timestamp_us",
        "gyro_x",
        "gyro_y",
        "gyro_z",
        "quat_w",
        "quat_x",
        "quat_y",
        "quat_z",
        "bias_x",
        "bias_y",
        "bias_z",
        "motor_0",
        "motor_1",
        "motor_2",
        "motor_3",
        "altitude_m",
        "vz_mps",
    ],
    rows: [
        [1000, 1, 2, 3, 1, 0, 0, 0, 0, 0, 0, 0.1, 0.2, 0.3, 0.4, 5, -1],
        [2000, 1, 2, 3, 1, 0, 0, 0, 0, 0, 0, 0.1, 0.2, 0.3, 0.4, 6, -2],
        [3000, 1, 2, 3, 1, 0, 0, 0, 0, 0, 0, 0.1, 0.2, 0.3, 0.4, 7, -3],
    ],
};

const SIM_RAW: Table = {
    total: 2,
    stride: 1,
    count: 2,
    columns: [
        "timestamp_us",
        "quat_w",
        "quat_x",
        "quat_y",
        "quat_z",
        "pos_x",
        "pos_y",
        "pos_z",
        "vel_x",
        "vel_y",
        "vel_z",
    ],
    rows: [
        [1500, 1, 0, 0, 0, 0, 0, 10, 0, 0, -1],
        [2500, 1, 0, 0, 0, 0, 0, 20, 0, 0, -2],
    ],
};

const LANES = [{ title: "vertical", keys: ["alt.est", "alt.baro", "alt.exact"] }];

test("fillFromStreams puts every series on the telemetry timestamps", () => {
    const filled = fillFromStreams(TELEMETRY, SIM_RAW, LANES);
    const alt = filled.buffers.get("alt.est");
    assert.ok(alt);
    assert.deepEqual(alt.t, [0, 0.001, 0.002]);
    assert.deepEqual(alt.v, [5, 6, 7]);
    assert.ok(Math.abs(filled.durationS - 0.002) < 1e-12);
});

test("fillFromStreams latches the exact state instead of pushing its rows", () => {
    const exact = fillFromStreams(TELEMETRY, SIM_RAW, LANES).buffers.get("alt.exact");
    assert.ok(exact);
    // No exact state before the first simRaw row, then the latest one at or
    // before each telemetry timestamp. The simRaw times never appear.
    assert.deepEqual(exact.t, [0, 0.001, 0.002]);
    assert.deepEqual(exact.v, [null, 10, 20]);
});

test("fillFromStreams agrees with the live sampler on the same input", () => {
    const replayed = fillFromStreams(TELEMETRY, SIM_RAW, LANES).buffers.get("alt.exact");
    const sampler = new LiveSampler();
    const live: (number | null)[] = [];
    // Same arrival order the hub would have produced on the wire
    live.push(sampler.sample(telemetryMessage(0)).values.get("alt.exact") ?? null);
    sampler.latchSimRaw(simRawMessage(0));
    live.push(sampler.sample(telemetryMessage(1)).values.get("alt.exact") ?? null);
    sampler.latchSimRaw(simRawMessage(1));
    live.push(sampler.sample(telemetryMessage(2)).values.get("alt.exact") ?? null);
    assert.deepEqual(replayed?.v, live);
});

test("fillFromStreams leaves the exact series empty without a simulator", () => {
    const filled = fillFromStreams(TELEMETRY, null, LANES);
    assert.deepEqual(filled.buffers.get("alt.exact")?.v, [null, null, null]);
    assert.deepEqual(filled.buffers.get("alt.est")?.v, [5, 6, 7]);
});

test("fillFromStreams empties the series a streams recording cannot carry", () => {
    // The streams CSV has no flight phase column: the series stays holes
    const filled = fillFromStreams(TELEMETRY, SIM_RAW, LANES);
    assert.deepEqual(filled.buffers.get("flightPhase")?.v, [null, null, null]);
    // TELEMETRY above is a recording made before baro_altitude_m existed.
    // It must still open, with that one series empty rather than an error.
    assert.deepEqual(filled.buffers.get("alt.baro")?.v, [null, null, null]);
});

const BLACKBOX: Table = {
    total: 2,
    stride: 1,
    count: 2,
    columns: [
        "timestamp_us",
        "gyro_x_rad_s",
        "accel_z_mps2",
        "baro_pa",
        "kill_switch",
        "throttle",
        "arm_switch",
        "motor_0",
    ],
    rows: [
        [0, 1, 9.8, 101325, 0, 0.5, 1, 0.25],
        [1000000, 2, 9.7, 101300, 1, 0.0, 1, 0],
    ],
};

test("fillFromBlackbox builds its lanes from the columns the hub sent", () => {
    const filled = fillFromBlackbox(BLACKBOX);
    assert.deepEqual(
        filled.lanes.map((lane) => lane.title),
        ["gyro", "accel", "baro", "motors", "pilot"]
    );
    assert.deepEqual(filled.buffers.get("gyro_x_rad_s")?.v, [1, 2]);
    assert.deepEqual(filled.buffers.get("gyro_x_rad_s")?.t, [0, 1]);
    assert.equal(filled.buffers.get("gyro_x_rad_s")?.def.unit, "rad/s");
    assert.equal(filled.buffers.get("accel_z_mps2")?.def.unit, "m/s^2");
    assert.equal(filled.buffers.get("baro_pa")?.def.unit, "Pa");
    assert.equal(filled.buffers.get("throttle")?.def.unit, "");
    assert.equal(filled.durationS, 1);
    // The timestamp is the x axis, never a series of its own
    assert.equal(filled.buffers.has("timestamp_us"), false);
});

/** The same row as the hub would have sent it over the websocket. */
function named(table: Table, row: number): Record<string, number> {
    const out: Record<string, number> = {};
    table.columns.forEach((name, i) => {
        out[name] = (table.rows[row] as number[])[i] as number;
    });
    return out;
}

function telemetryMessage(row: number): Record<string, unknown> {
    const c = named(TELEMETRY, row);
    return {
        timestampUs: c["timestamp_us"],
        gyroRadS: [c["gyro_x"], c["gyro_y"], c["gyro_z"]],
        attitudeQuat: [c["quat_w"], c["quat_x"], c["quat_y"], c["quat_z"]],
        motor: [c["motor_0"], c["motor_1"], c["motor_2"], c["motor_3"]],
        altitudeM: c["altitude_m"],
        verticalVelocityMps: c["vz_mps"],
    };
}

function simRawMessage(row: number): Record<string, unknown> {
    const c = named(SIM_RAW, row);
    return {
        timestampUs: c["timestamp_us"],
        attitudeQuat: [c["quat_w"], c["quat_x"], c["quat_y"], c["quat_z"]],
        positionM: [c["pos_x"], c["pos_y"], c["pos_z"]],
        velocityMps: [c["vel_x"], c["vel_y"], c["vel_z"]],
    };
}
