import assert from "node:assert/strict";
import test from "node:test";

import { groupByLane, moveLane, SeriesBuffer } from "../src/lanes/model";
import { create } from "@bufbuild/protobuf";

import { StatusSchema } from "../src/gen/mark4_pb";
import { sampleStatus, seriesByKey, type SeriesDef } from "../src/shared/series";

function def(key: string): SeriesDef {
    const found = seriesByKey(key);
    assert.ok(found, `unknown series ${key}`);
    return found;
}

test("SeriesBuffer keeps samples in arrival order", () => {
    const buffer = new SeriesBuffer(def("motor.0"), 10);
    assert.ok(buffer.push(0, 1));
    assert.ok(buffer.push(1, 2));
    assert.deepEqual(buffer.t, [0, 1]);
    assert.deepEqual(buffer.v, [1, 2]);
    assert.equal(buffer.last(), 2);
    assert.equal(buffer.endS(), 1);
});

test("SeriesBuffer drops samples that do not move forward in time", () => {
    const buffer = new SeriesBuffer(def("motor.0"), 10);
    buffer.push(1, 10);
    assert.equal(buffer.push(1, 11), false);
    assert.equal(buffer.push(0.5, 12), false);
    assert.deepEqual(buffer.v, [10]);
});

test("SeriesBuffer drops the oldest samples past its capacity", () => {
    const capacity = 100;
    const buffer = new SeriesBuffer(def("motor.0"), capacity);
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
        ["motor.0", new SeriesBuffer(def("motor.0"))],
        ["motor.1", new SeriesBuffer(def("motor.1"))],
    ]);
    const grouped = groupByLane(
        [
            { title: "motors", keys: ["motor.1", "motor.0"] },
            { title: "nothing here", keys: ["alt.exact"] },
        ],
        buffers
    );
    assert.equal(grouped.length, 1);
    assert.equal(grouped[0]?.config.title, "motors");
    assert.deepEqual(
        grouped[0]?.buffers.map((b) => b.def.key),
        ["motor.1", "motor.0"]
    );
});

const STATUS_INIT = {
    timestampUs: 2000n,
    attitudeQuat: [1, 0, 0, 0],
    motor: [0.1, 0.2, 0.3, 0.4],
    flightPhase: 5,
    throwState: 2,
};
const STATUS = create(StatusSchema, STATUS_INIT);

const TRUTH = {
    attitudeQuat: [Math.SQRT1_2, 0, 0, Math.SQRT1_2],
    positionM: [1, 2, 7],
    velocityMps: [0, 0, -2],
};

test("sampleStatus leaves the exact series empty without a truth", () => {
    const row = sampleStatus(STATUS);
    assert.equal(row.timestampUs, 2000);
    assert.equal(row.values.get("motor.1"), 0.2);
    assert.equal(row.values.get("alt.exact"), null);
    assert.equal(row.values.get("attitude.error"), null);
});

test("sampleStatus reads the exact state out of the truth the message carries", () => {
    const row = sampleStatus(create(StatusSchema, { ...STATUS_INIT, truth: TRUTH }));
    assert.equal(row.timestampUs, 2000);
    assert.equal(row.values.get("alt.exact"), 7);
    assert.equal(row.values.get("vz.exact"), -2);
    assert.ok(Math.abs((row.values.get("euler.exact.yaw") as number) - 90) < 1e-9);
    assert.ok(Math.abs((row.values.get("attitude.error") as number) - 90) < 1e-9);
});

test("sampleStatus ignores a truth without an attitude", () => {
    const row = sampleStatus(
        create(StatusSchema, { ...STATUS_INIT, truth: { positionM: [0, 0, 9] } })
    );
    assert.equal(row.values.get("alt.exact"), null);
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
