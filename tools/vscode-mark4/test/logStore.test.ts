import assert from "node:assert/strict";
import { test } from "node:test";

import { LogLevel } from "../src/gen/mark4_pb";
import { type LogRecord, LogStore } from "../src/logStore";

const record = (text: string): LogRecord => ({
    receivedAt: new Date(2026, 7, 30, 12, 0, 0, 0),
    nodeId: 0xd5000001,
    moduleId: 1,
    level: LogLevel.INFO,
    text,
});

test("the store keeps what it received, oldest first", () => {
    const store = new LogStore(4);
    store.push(record("a"));
    store.push(record("b"));
    assert.equal(store.size, 2);
    assert.deepEqual(
        store.records().map((item) => item.text),
        ["a", "b"],
    );
});

test("a full store drops the oldest line and keeps the order", () => {
    const store = new LogStore(3);
    for (const text of ["a", "b", "c", "d", "e"]) {
        store.push(record(text));
    }
    assert.equal(store.size, 3);
    assert.deepEqual(
        store.records().map((item) => item.text),
        ["c", "d", "e"],
    );
});

test("clearing empties the ring back to its start", () => {
    const store = new LogStore(2);
    store.push(record("a"));
    store.push(record("b"));
    store.push(record("c"));
    store.clear();
    assert.equal(store.size, 0);
    store.push(record("d"));
    assert.deepEqual(
        store.records().map((item) => item.text),
        ["d"],
    );
});
