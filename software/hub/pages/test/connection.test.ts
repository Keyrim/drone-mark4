import assert from "node:assert/strict";
import test from "node:test";

import { NO_CONNECTION, candidateRows, type Connection } from "../src/console/connection";

const SIM = { kind: 2, kindName: "drone_sim" };
const BOARD = { kind: 1, kindName: "firmware" };
const BRIDGE = { address: "192.168.1.31", port: 47830, name: "c19f6c" };

test("every route is one list, nothing is connected without a click", () => {
    const rows = candidateRows([SIM], [BRIDGE], NO_CONNECTION);
    assert.equal(rows.length, 2);
    assert.equal(rows[0]!.state, "available");
    assert.deepEqual(rows[0]!.connect, { type: "connect", via: "udp", target: "drone_sim" });
    assert.equal(rows[1]!.state, "available");
    assert.deepEqual(rows[1]!.connect, { type: "connect", via: "bridge", name: "c19f6c" });
});

test("the board is evidence, not a udp candidate", () => {
    const rows = candidateRows([BOARD], [], NO_CONNECTION);
    assert.equal(rows.length, 0);
});

test("the connected row is marked and offers no connect", () => {
    const connected: Connection = {
        via: "udp",
        id: "drone_sim",
        kind: 2,
        kindName: "drone_sim",
        live: true,
    };
    const rows = candidateRows([SIM], [BRIDGE], connected);
    assert.equal(rows[0]!.state, "connected");
    assert.equal(rows[0]!.connect, null);
    assert.equal(rows[1]!.state, "available");
});

test("a lost drone keeps its row instead of vanishing", () => {
    const connected: Connection = {
        via: "udp",
        id: "drone_sim",
        kind: 2,
        kindName: "drone_sim",
        live: false,
    };
    const rows = candidateRows([], [], connected);
    assert.equal(rows.length, 1);
    assert.equal(rows[0]!.label, "drone_sim");
    assert.equal(rows[0]!.state, "lost");
    assert.equal(rows[0]!.connect, null);
});

test("a bridge whose board is silent shows the connection as lost", () => {
    const connected: Connection = {
        via: "bridge",
        id: "c19f6c",
        kind: 1,
        kindName: "firmware",
        live: false,
    };
    const rows = candidateRows([], [BRIDGE], connected);
    assert.equal(rows.length, 1);
    assert.equal(rows[0]!.state, "lost");
});

test("a nameless bridge is listed but not connectable", () => {
    const rows = candidateRows([], [{ ...BRIDGE, name: "" }], NO_CONNECTION);
    assert.equal(rows.length, 1);
    assert.equal(rows[0]!.connect, null);
});
