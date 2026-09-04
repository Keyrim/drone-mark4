import assert from "node:assert/strict";
import test from "node:test";

import { TelemetryUnit } from "../src/gen/mark4_pb";
import { clampPeriod, DEFAULT_PERIOD_MS, MAX_PERIOD_MS, MIN_PERIOD_MS } from "../src/telemetry/config_panel";
import { MAX_POINTS_PER_SERIES, nextColor, TelemetryModel, unitLabel, type Descriptor } from "../src/telemetry/model";

const GYRO_X: Descriptor = { id: 0, name: "sensor/gyro_x", unit: TelemetryUnit.RAD_PER_S };
const GYRO_Y: Descriptor = { id: 1, name: "sensor/gyro_y", unit: TelemetryUnit.RAD_PER_S };
const ALTITUDE: Descriptor = { id: 2, name: "estimator/altitude", unit: TelemetryUnit.M };
const TABLE = [GYRO_X, GYRO_Y, ALTITUDE];

const T0 = 1_000_000;

function model(...descriptors: Descriptor[]): TelemetryModel {
    const built = new TelemetryModel();
    for (const descriptor of descriptors) {
        built.add(descriptor);
    }
    return built;
}

test("a selection is routed by id and identified by name", () => {
    const m = model(GYRO_X, ALTITUDE);
    assert.deepEqual(
        m.list().map((series) => series.name),
        ["sensor/gyro_x", "estimator/altitude"]
    );
    assert.deepEqual(m.enabledIds(), [0, 2]);

    m.ingest(T0, [
        { id: 0, value: 1.5 },
        { id: 2, value: -3 },
    ]);
    assert.deepEqual(m.buffer("sensor/gyro_x")?.v, [1.5]);
    assert.deepEqual(m.buffer("estimator/altitude")?.v, [-3]);
    // t is seconds from the first sample: the first one sits at 0.
    assert.deepEqual(m.buffer("sensor/gyro_x")?.t, [0]);
    assert.equal(m.originUs(), T0);
});

test("selecting the same measure twice changes nothing", () => {
    const m = model(GYRO_X, GYRO_X);
    assert.equal(m.list().length, 1);
});

test("a new selection opens in a lane of its own, with an unused hue", () => {
    const m = model(GYRO_X, GYRO_Y, ALTITUDE);
    const lanes = m.list().map((series) => series.laneId);
    assert.equal(new Set(lanes).size, 3);
    const colors = m.list().map((series) => series.color);
    assert.equal(new Set(colors).size, 3);
    assert.equal(nextColor(m.list()) === colors[0], false);
});

test("series of the same unit can be grouped, and lanes reordered", () => {
    const m = model(GYRO_X, GYRO_Y, ALTITUDE);
    const [x, y] = m.lanes();
    m.setLane("sensor/gyro_y", x as number);
    assert.equal(m.lanes().length, 2);
    assert.equal(
        m.list().filter((series) => series.laneId === x).length,
        2
    );
    assert.notEqual(y, undefined);

    // The order the lanes are asked for is the order they end up in, and the
    // ids are renumbered from 0 with no gap left behind.
    const order = [...m.lanes()].reverse();
    m.orderLanes(order);
    assert.deepEqual(m.lanes(), [0, 1]);
    assert.equal(m.list().find((series) => series.name === "estimator/altitude")?.laneId, 0);

    // -1 asks for a lane of its own again.
    m.setLane("sensor/gyro_y", -1);
    assert.equal(m.lanes().length, 3);
});

test("a group opens one lane per unit, joining the lane a member already has", () => {
    const m = model(GYRO_X);
    const lane = m.list()[0]?.laneId;
    m.addGroup([GYRO_X, GYRO_Y, ALTITUDE]);
    assert.deepEqual(
        m.list().map((series) => series.name),
        ["sensor/gyro_x", "sensor/gyro_y", "estimator/altitude"]
    );
    // gyro_y reads like gyro_x and joins its lane; altitude reads in
    // metres and gets one of its own.
    assert.equal(m.list()[1]?.laneId, lane);
    assert.equal(m.lanes().length, 2);
    // Ticking the group again changes nothing.
    m.addGroup([GYRO_X, GYRO_Y, ALTITUDE]);
    assert.equal(m.list().length, 3);
});

test("routes are rebuilt from the node's table, by name", () => {
    const m = model(GYRO_X, ALTITUDE);

    // The same node after a reboot: the same names, other ids.
    m.bind([
        { id: 7, name: "estimator/altitude", unit: TelemetryUnit.M },
        { id: 9, name: "sensor/gyro_x", unit: TelemetryUnit.RAD_PER_S },
    ]);
    assert.deepEqual(m.enabledIds(), [7, 9]);
    assert.equal(m.isStale("sensor/gyro_x"), false);

    m.ingest(T0, [{ id: 9, value: 2 }]);
    assert.deepEqual(m.buffer("sensor/gyro_x")?.v, [2]);
    // The old id feeds nothing any more.
    m.ingest(T0 + 1000, [{ id: 0, value: 99 }]);
    assert.deepEqual(m.buffer("sensor/gyro_x")?.v, [2]);
});

test("a measure the node no longer exposes is flagged stale and fed nothing", () => {
    const m = model(GYRO_X, ALTITUDE);
    m.bind([GYRO_X]);
    assert.equal(m.isStale("estimator/altitude"), true);
    assert.equal(m.isStale("sensor/gyro_x"), false);
    assert.deepEqual(m.enabledIds(), [0]);

    // And it comes back when the name is back, at whatever id.
    m.bind(TABLE);
    assert.equal(m.isStale("estimator/altitude"), false);
    assert.deepEqual(m.enabledIds(), [0, 2]);
});

test("a sample whose time does not move forward is dropped whole", () => {
    const m = model(GYRO_X);
    assert.equal(m.ingest(T0, [{ id: 0, value: 1 }]), true);
    assert.equal(m.ingest(T0, [{ id: 0, value: 2 }]), false);
    assert.equal(m.ingest(T0 - 1000, [{ id: 0, value: 3 }]), false);
    assert.deepEqual(m.buffer("sensor/gyro_x")?.v, [1]);
    assert.equal(m.ingest(T0 + 1000, [{ id: 0, value: 4 }]), true);
    assert.deepEqual(m.buffer("sensor/gyro_x")?.v, [1, 4]);
});

test("a gap marker breaks every curve without folding the time base", () => {
    const m = model(GYRO_X, ALTITUDE);
    m.ingest(T0, [
        { id: 0, value: 1 },
        { id: 2, value: 5 },
    ]);
    m.markGap();
    assert.equal(m.buffer("sensor/gyro_x")?.last(), null);
    assert.equal(m.buffer("estimator/altitude")?.last(), null);

    // Samples after the hole still land, so the marker did not eat the
    // ascending contract of the buffers.
    assert.equal(m.ingest(T0 + 100_000, [{ id: 0, value: 7 }]), true);
    assert.deepEqual(m.buffer("sensor/gyro_x")?.v, [1, null, 7]);

    // A gap before any sample is nothing to break.
    const empty = model(GYRO_X);
    empty.markGap();
    assert.equal(empty.buffer("sensor/gyro_x")?.t.length, 0);
});

test("a buffer is capped and drops its oldest half", () => {
    const m = model(GYRO_X);
    for (let i = 0; i <= MAX_POINTS_PER_SERIES; i++) {
        m.ingest(T0 + i * 1000, [{ id: 0, value: i }]);
    }
    const buffer = m.buffer("sensor/gyro_x");
    assert.ok(buffer);
    assert.ok(buffer.t.length <= MAX_POINTS_PER_SERIES);
    // Roughly half the ring is left, and the newest sample survived.
    assert.ok(buffer.t.length >= MAX_POINTS_PER_SERIES / 2 - 1);
    assert.equal(buffer.v[buffer.v.length - 1], MAX_POINTS_PER_SERIES);
});

test("removing a series takes its buffer and its route with it", () => {
    const m = model(GYRO_X, ALTITUDE);
    m.remove("sensor/gyro_x");
    assert.equal(m.has("sensor/gyro_x"), false);
    assert.equal(m.buffer("sensor/gyro_x"), undefined);
    assert.deepEqual(m.enabledIds(), [2]);
});

test("clearData keeps the selection, reset drops it", () => {
    const m = model(GYRO_X);
    m.ingest(T0, [{ id: 0, value: 1 }]);
    m.clearData();
    assert.equal(m.list().length, 1);
    assert.equal(m.originUs(), null);
    assert.equal(m.durationS(), 0);
    m.reset();
    assert.equal(m.list().length, 0);
});

test("a unit has one axis label, and the unitless one has none", () => {
    assert.equal(unitLabel(TelemetryUnit.RAD_PER_S), "rad/s");
    assert.equal(unitLabel(TelemetryUnit.UNITLESS), "");
    assert.equal(unitLabel(TelemetryUnit.CELSIUS), "degC");
    assert.equal(unitLabel(TelemetryUnit.MAH), "mAh");
});

test("the period is clamped to what a subscriber may ask for", () => {
    assert.equal(clampPeriod(0), MIN_PERIOD_MS);
    assert.equal(clampPeriod(-5), MIN_PERIOD_MS);
    assert.equal(clampPeriod(1e9), MAX_PERIOD_MS);
    assert.equal(clampPeriod(50), 50);
    assert.equal(clampPeriod(50.4), 50);
    assert.equal(clampPeriod(Number.NaN), DEFAULT_PERIOD_MS);
});
