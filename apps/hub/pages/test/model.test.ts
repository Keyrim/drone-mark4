import assert from "node:assert/strict";
import test from "node:test";

import { groupByLane, moveLane, SeriesBuffer } from "../src/lanes/model";
import { LiveSampler, seriesByKey, type SeriesDef } from "../src/shared/series";

function def(key: string): SeriesDef {
    const found = seriesByKey(key);
    assert.ok(found, `unknown series ${key}`);
    return found;
}

test("SeriesBuffer keeps samples in arrival order", () => {
    const buffer = new SeriesBuffer(def("gyro.x"), 10);
    assert.ok(buffer.push(0, 1));
    assert.ok(buffer.push(1, 2));
    assert.deepEqual(buffer.t, [0, 1]);
    assert.deepEqual(buffer.v, [1, 2]);
    assert.equal(buffer.last(), 2);
    assert.equal(buffer.endS(), 1);
});

test("SeriesBuffer drops samples that do not move forward in time", () => {
    const buffer = new SeriesBuffer(def("gyro.x"), 10);
    buffer.push(1, 10);
    assert.equal(buffer.push(1, 11), false);
    assert.equal(buffer.push(0.5, 12), false);
    assert.deepEqual(buffer.v, [10]);
});

test("SeriesBuffer drops the oldest samples past its capacity", () => {
    const capacity = 100;
    const buffer = new SeriesBuffer(def("gyro.x"), capacity);
    for (let i = 0; i < 300; i++) {
        buffer.push(i, i);
    }
    assert.ok(buffer.t.length <= capacity, `length ${buffer.t.length}`);
    // The newest sample is always there, the oldest ones are gone
    assert.equal(buffer.t[buffer.t.length - 1], 299);
    assert.equal(buffer.v[buffer.v.length - 1], 299);
    assert.ok((buffer.t[0] as number) > 0);
    // The window stays contiguous and ascending
    assert.equal((buffer.t[buffer.t.length - 1] as number) - (buffer.t[0] as number), buffer.t.length - 1);
});

test("SeriesBuffer stores null as a hole and clears", () => {
    const buffer = new SeriesBuffer(def("alt.exact"), 10);
    buffer.push(0, 1);
    buffer.push(1, null);
    assert.equal(buffer.last(), null);
    buffer.clear();
    assert.equal(buffer.t.length, 0);
    assert.equal(buffer.endS(), 0);
});

test("groupByLane keeps the config order and drops what it cannot feed", () => {
    const buffers = new Map([
        ["gyro.x", new SeriesBuffer(def("gyro.x"))],
        ["gyro.y", new SeriesBuffer(def("gyro.y"))],
    ]);
    const grouped = groupByLane(
        [
            { title: "gyro", keys: ["gyro.y", "gyro.x"] },
            { title: "nothing here", keys: ["alt.exact"] },
        ],
        buffers
    );
    assert.equal(grouped.length, 1);
    assert.equal(grouped[0]?.config.title, "gyro");
    assert.deepEqual(
        grouped[0]?.buffers.map((b) => b.def.key),
        ["gyro.y", "gyro.x"]
    );
});

const TELEMETRY = {
    type: "telemetry",
    timestampUs: 2000,
    gyroRadS: [1, 2, 3],
    attitudeQuat: [1, 0, 0, 0],
    motor: [0.1, 0.2, 0.3, 0.4],
    altitudeM: 5,
    verticalVelocityMps: -1,
    flightPhase: 5,
    throwState: 2,
};

const SIM_RAW = {
    type: "simRaw",
    timestampUs: 1000,
    attitudeQuat: [Math.SQRT1_2, 0, 0, Math.SQRT1_2],
    positionM: [1, 2, 7],
    velocityMps: [0, 0, -2],
};

test("LiveSampler leaves the exact series empty until a simRaw lands", () => {
    const sampler = new LiveSampler();
    const row = sampler.sample(TELEMETRY);
    assert.equal(row.timestampUs, 2000);
    assert.equal(row.values.get("gyro.y"), 2);
    assert.equal(row.values.get("alt.est"), 5);
    assert.equal(row.values.get("alt.exact"), null);
    assert.equal(row.values.get("attitude.error"), null);
    assert.equal(sampler.latchGapUs(2000), null);
});

test("LiveSampler samples the latched exact state on the telemetry row", () => {
    const sampler = new LiveSampler();
    sampler.latchSimRaw(SIM_RAW);
    const row = sampler.sample(TELEMETRY);
    // The exact values ride the telemetry timestamp, not their own
    assert.equal(row.timestampUs, 2000);
    assert.equal(row.values.get("alt.exact"), 7);
    assert.equal(row.values.get("vz.exact"), -2);
    assert.ok(Math.abs((row.values.get("euler.exact.yaw") as number) - 90) < 1e-9);
    assert.ok(Math.abs((row.values.get("attitude.error") as number) - 90) < 1e-9);
    assert.equal(sampler.latchGapUs(2000), 1000);
});

test("LiveSampler keeps the newest latch only", () => {
    const sampler = new LiveSampler();
    sampler.latchSimRaw(SIM_RAW);
    sampler.latchSimRaw({ ...SIM_RAW, timestampUs: 1900, positionM: [0, 0, 9] });
    assert.equal(sampler.sample(TELEMETRY).values.get("alt.exact"), 9);
    assert.equal(sampler.latchGapUs(2000), 100);
});

test("LiveSampler ignores a simRaw without an attitude", () => {
    const sampler = new LiveSampler();
    sampler.latchSimRaw({ type: "simRaw", timestampUs: 1 });
    assert.equal(sampler.sample(TELEMETRY).values.get("alt.exact"), null);
});

test("moveLane moves a lane to the drop index", () => {
    const lanes = [
        { title: "a", keys: [] },
        { title: "b", keys: [] },
        { title: "c", keys: [] },
    ];
    moveLane(lanes, 0, 2);
    assert.deepEqual(lanes.map((lane) => lane.title), ["b", "c", "a"]);
    moveLane(lanes, 2, 0);
    assert.deepEqual(lanes.map((lane) => lane.title), ["a", "b", "c"]);
});

test("moveLane ignores an index outside the array", () => {
    const lanes = [
        { title: "a", keys: [] },
        { title: "b", keys: [] },
    ];
    moveLane(lanes, 0, 5);
    moveLane(lanes, -1, 1);
    moveLane(lanes, 1, 1);
    assert.deepEqual(lanes.map((lane) => lane.title), ["a", "b"]);
});
