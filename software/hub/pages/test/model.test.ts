import assert from "node:assert/strict";
import test from "node:test";

import { groupByLane, moveLane, SeriesBuffer, type SeriesDef } from "../src/lanes/model";

function def(key: string): SeriesDef {
    return { key, label: key, unit: "", color: "#3987e5" };
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
