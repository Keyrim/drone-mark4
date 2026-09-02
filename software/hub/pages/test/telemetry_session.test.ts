import assert from "node:assert/strict";
import test from "node:test";

import { TelemetryUnit } from "../src/gen/mark4_pb";
import { TelemetryModel, type Descriptor, type SeriesSpec } from "../src/telemetry/model";
import {
    buildConfig,
    buildSession,
    CONFIG_VERSION,
    parseConfig,
    parseSession,
    SESSION_VERSION,
    toCsv,
} from "../src/telemetry/session";

const NODE = { id: 0x1234abcd, kind: "drone_sim", label: "drone_sim 1234abcd" };

const SPECS: SeriesSpec[] = [
    { name: "estimator/altitude", unit: TelemetryUnit.M, color: "#3987e5", laneId: 0 },
    { name: "throw/count", unit: TelemetryUnit.COUNT, color: "#199e70", laneId: 1 },
];

const DATA = [
    { t: [0, 0.05, 0.1], v: [1.5, null, 2.5] },
    { t: [0, 0.05], v: [0, 1] },
];

test("a session round trips through its stored form", () => {
    const session = buildSession("throw_12", NODE, 7_000_000, 0.1, 50, SPECS, DATA);
    assert.equal(session.version, SESSION_VERSION);
    const back = parseSession(JSON.stringify(session));
    assert.ok(back);
    assert.equal(back.name, "throw_12");
    assert.equal(back.node.id, NODE.id);
    assert.equal(back.t0Us, 7_000_000);
    assert.equal(back.periodMs, 50);
    assert.equal(back.series.length, 2);
    assert.equal(back.series[0]?.name, "estimator/altitude");
    assert.deepEqual(back.series[0]?.v, [1.5, null, 2.5]);
    assert.equal(back.series[1]?.laneId, 1);
});

test("a stored session opens into the model, gap markers and lanes included", () => {
    const session = buildSession("throw_12", NODE, 7_000_000, 0.1, 50, SPECS, DATA);
    const parsed = parseSession(JSON.stringify(session));
    assert.ok(parsed);
    const model = new TelemetryModel();
    model.load(parsed.series, parsed.series);
    assert.deepEqual(
        model.list().map((series) => series.name),
        ["estimator/altitude", "throw/count"]
    );
    assert.deepEqual(model.buffer("estimator/altitude")?.v, [1.5, null, 2.5]);
    assert.deepEqual(model.lanes(), [0, 1]);
    assert.equal(model.durationS(), 0.1);
    // Nothing is bound until it is bound to a node: a session is browsed.
    assert.deepEqual(model.enabledIds(), []);
});

test("what parseSession refuses", () => {
    assert.equal(parseSession("not json"), null);
    assert.equal(parseSession("[]"), null);
    assert.equal(parseSession(JSON.stringify({ version: 99, series: [] })), null);
    // A series whose arrays do not line up would draw values at the wrong
    // instants: refused whole rather than half loaded.
    assert.equal(
        parseSession(
            JSON.stringify({ version: SESSION_VERSION, series: [{ name: "a", t: [0, 1], v: [1] }] })
        ),
        null
    );
    assert.equal(
        parseSession(
            JSON.stringify({ version: SESSION_VERSION, series: [{ name: "a", t: [0], v: ["x"] }] })
        ),
        null
    );
    assert.equal(
        parseSession(JSON.stringify({ version: SESSION_VERSION, series: [{ t: [], v: [] }] })),
        null
    );
    // A document with no samples at all is still a session.
    const bare = parseSession(JSON.stringify({ version: SESSION_VERSION, series: [] }));
    assert.ok(bare);
    assert.equal(bare.series.length, 0);
});

test("a view config carries what is ticked and no data", () => {
    const config = buildConfig(20, SPECS);
    assert.equal(config.version, CONFIG_VERSION);
    assert.equal(config.periodMs, 20);
    assert.deepEqual(Object.keys(config.series[0] ?? {}), ["name", "unit", "color", "laneId"]);

    const back = parseConfig(JSON.stringify(config));
    assert.ok(back);
    assert.equal(back.series.length, 2);
    assert.equal(back.series[1]?.name, "throw/count");
    assert.equal(back.periodMs, 20);
});

test("what parseConfig refuses", () => {
    assert.equal(parseConfig("{"), null);
    assert.equal(parseConfig(JSON.stringify({ version: 99, series: [] })), null);
    assert.equal(parseConfig(JSON.stringify({ version: CONFIG_VERSION, series: [{}] })), null);
    // A config with no period falls back to the page's default rather than
    // asking a node for a period of 0, which would stop the stream.
    const bare = parseConfig(JSON.stringify({ version: CONFIG_VERSION, series: [] }));
    assert.ok(bare);
    assert.equal(bare.periodMs, 50);
});

test("the csv is long format and skips the gap markers", () => {
    const csv = toCsv(SPECS, DATA);
    const rows = csv.trimEnd().split("\n");
    assert.equal(rows[0], "series,unit,t_s,value");
    // Three samples for the first series minus the one hole, two for the
    // second: a hole is the absence of a value, not a value.
    assert.equal(rows.length, 1 + 2 + 2);
    assert.equal(rows[1], "estimator/altitude,m,0,1.5");
    assert.equal(rows[2], "estimator/altitude,m,0.1,2.5");
    assert.equal(rows[3], "throw/count,count,0,0");
    assert.ok(csv.endsWith("\n"));
});

test("a series name that needs quoting is quoted", () => {
    const odd: SeriesSpec[] = [
        { name: 'weird,"name', unit: TelemetryUnit.UNITLESS, color: "#000", laneId: 0 },
    ];
    const csv = toCsv(odd, [{ t: [0], v: [1] }]);
    assert.ok(csv.includes('"weird,""name",,0,1'));
});

test("a descriptor list drives the selection, no catalog anywhere", () => {
    // The whole point of the registry: the page offers what the node says
    // it has, whatever that is, with no list of its own.
    const invented: Descriptor[] = [
        { id: 4, name: "brand/new_measure", unit: TelemetryUnit.V },
    ];
    const model = new TelemetryModel();
    model.add(invented[0] as Descriptor);
    assert.equal(model.has("brand/new_measure"), true);
    assert.deepEqual(model.enabledIds(), [4]);
});
