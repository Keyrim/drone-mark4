import assert from "node:assert/strict";
import test from "node:test";

import { TelemetryUnit } from "../src/gen/mark4_pb";
import { buildConfig, CONFIG_VERSION, exportName, parseConfig, toCsv } from "../src/telemetry/config";
import { TelemetryModel, type Descriptor, type SeriesSpec } from "../src/telemetry/model";

const SPECS: SeriesSpec[] = [
    { name: "estimator/altitude", unit: TelemetryUnit.M, color: "#3987e5", laneId: 0 },
    { name: "throw/count", unit: TelemetryUnit.COUNT, color: "#199e70", laneId: 1 },
];

const DATA = [
    { t: [0, 0.05, 0.1], v: [1.5, null, 2.5] },
    { t: [0, 0.05], v: [0, 1] },
];

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

test("an export is named after its config and its instant", () => {
    const at = new Date(2026, 8, 3, 14, 7, 9);
    assert.equal(exportName("rate-tuning", at), "rate-tuning-20260903-140709");
    // No config name: a generic stem, never an empty one.
    assert.equal(exportName("", at), "telemetry-20260903-140709");
    // Only what the hub takes as a file name survives.
    assert.equal(exportName("odd name.v2", at), "oddnamev2-20260903-140709");
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
