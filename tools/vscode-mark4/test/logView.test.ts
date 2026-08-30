import assert from "node:assert/strict";
import { test } from "node:test";

import { LogLevel } from "../src/gen/mark4_pb";
import { type LogRecord } from "../src/logStore";
import { formatLogLine, LogFilter, type NameTable, renderLogs, sameNames } from "../src/logView";

const SIM = 0xd5000001;
const HUB = 0x0000000a;

const at = (ms: number): Date => new Date(2026, 7, 30, 12, 34, 56, ms);

const record = (fields: Partial<LogRecord> = {}): LogRecord => ({
    receivedAt: at(0),
    nodeId: SIM,
    moduleId: 1,
    level: LogLevel.INFO,
    text: "up",
    ...fields,
});

const names: NameTable = new Map([
    [SIM, { kind: "drone_sim", modules: new Map([[1, "sim/link"]]) }],
    [HUB, { kind: "gateway", modules: new Map([[7, "gateway/ws"]]) }],
]);

test("a line owns its timestamp and its columns", () => {
    assert.equal(
        formatLogLine(at(7), LogLevel.INFO, "drone_sim", SIM, "sim/link", "up"),
        "12:34:56.007  INFO   drone_sim  d5000001  sim/link                  up",
    );
    // Every level is 5 characters, so the columns after it never move
    for (const level of [LogLevel.TRACE, LogLevel.DEBUG, LogLevel.INFO, LogLevel.WARN, LogLevel.ERROR]) {
        assert.equal(formatLogLine(at(0), level, "firmware", 1, "m", "x").indexOf("firmware"), 21);
    }
    // A long module is cut to keep the text column; the kind never is
    assert.match(formatLogLine(at(0), LogLevel.WARN, "drone_sim", 1, "a".repeat(30), "x"), /  a{24}  x$/);
    assert.match(formatLogLine(at(0), LogLevel.WARN, "kind 99", 1, "m", "x"), /  kind 99    00000001  /);
});

test("names resolve at render time, so a late table names old lines", () => {
    const records = [record(), record({ nodeId: HUB, moduleId: 7, text: "client" })];
    const before = renderLogs(records, new Map(), new LogFilter());
    assert.match(before, /unknown    d5000001  #1 /);
    assert.match(before, /unknown    0000000a  #7 /);
    const after = renderLogs(records, names, new LogFilter());
    assert.match(after, /drone_sim  d5000001  sim\/link /);
    assert.match(after, /gateway    0000000a  gateway\/ws /);
});

test("the display filter starts at INFO, per (node, module)", () => {
    const records = [
        record({ level: LogLevel.DEBUG, text: "quiet" }),
        record({ level: LogLevel.INFO, text: "loud" }),
    ];
    const filter = new LogFilter();
    assert.equal(renderLogs(records, names, filter).split("\n").length, 1);
    assert.match(renderLogs(records, names, filter), /loud$/);
    filter.setLevel([{ nodeId: SIM, moduleId: 1 }], LogLevel.DEBUG);
    assert.equal(renderLogs(records, names, filter).split("\n").length, 2, "lowering shows the stored lines");
    filter.setLevel([{ nodeId: SIM, moduleId: 1 }], LogLevel.ERROR);
    assert.equal(renderLogs(records, names, filter), "");
    // Another module of the same node keeps its own default
    assert.equal(renderLogs([record({ moduleId: 2 })], names, filter).split("\n").length, 1);
});

test("a hidden node drops out of the view whatever its levels", () => {
    const records = [record(), record({ nodeId: HUB, moduleId: 7, text: "client" })];
    const filter = new LogFilter();
    assert.equal(filter.toggleHidden(SIM), true);
    assert.equal(filter.isHidden(SIM), true);
    assert.match(renderLogs(records, names, filter), /^\S+  INFO   gateway /);
    assert.equal(filter.toggleHidden(SIM), false);
    assert.equal(renderLogs(records, names, filter).split("\n").length, 2);
});

test("the search runs on the whole line, case insensitive", () => {
    const records = [record({ text: "armed" }), record({ nodeId: HUB, moduleId: 7, text: "client" })];
    assert.match(renderLogs(records, names, new LogFilter(), "ARMED"), /armed$/);
    assert.equal(renderLogs(records, names, new LogFilter(), "armed").split("\n").length, 1);
    assert.equal(renderLogs(records, names, new LogFilter(), "gateway/ws").split("\n").length, 1);
    assert.equal(renderLogs(records, names, new LogFilter(), "0000000a").split("\n").length, 1);
    assert.equal(renderLogs(records, names, new LogFilter(), "nothing"), "");
});

test("two name tables are the same when every line would read the same", () => {
    assert.equal(sameNames(names, new Map(names)), true);
    assert.equal(sameNames(names, new Map([...names].slice(1))), false);
    const renamed: NameTable = new Map([
        [SIM, { kind: "drone_sim", modules: new Map([[1, "sim/uplink"]]) }],
        [HUB, { kind: "gateway", modules: new Map([[7, "gateway/ws"]]) }],
    ]);
    assert.equal(sameNames(names, renamed), false);
    const rekinded: NameTable = new Map([
        [SIM, { kind: "firmware", modules: new Map([[1, "sim/link"]]) }],
        [HUB, { kind: "gateway", modules: new Map([[7, "gateway/ws"]]) }],
    ]);
    assert.equal(sameNames(names, rekinded), false);
});
