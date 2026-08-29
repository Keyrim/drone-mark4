import assert from "node:assert/strict";
import test from "node:test";

import { NO_CONNECTION, candidateRows, type Connection } from "../src/console/connection";

const SIM = { kind: 2, kindName: "drone_sim" };
const BOARD = { kind: 1, kindName: "firmware" };

test("every announced node is one row, nothing is connected without a click", () => {
    const rows = candidateRows([SIM, BOARD], NO_CONNECTION);
    assert.equal(rows.length, 2);
    assert.equal(rows[0]!.state, "available");
    assert.deepEqual(rows[0]!.connect, { type: "connect", via: "udp", target: "drone_sim" });
    assert.equal(rows[1]!.state, "available");
    assert.deepEqual(rows[1]!.connect, { type: "connect", via: "udp", target: "firmware" });
});

test("a plant on the LAN is not a drone and gets no row", () => {
    const rows = candidateRows([{ kind: 3, kindName: "plant" }, SIM], NO_CONNECTION);
    assert.equal(rows.length, 1);
    assert.equal(rows[0]!.label, "drone_sim");
});

test("the connected row is marked and offers no connect", () => {
    const connected: Connection = {
        via: "udp",
        id: "drone_sim",
        kind: 2,
        kindName: "drone_sim",
        live: true,
    };
    const rows = candidateRows([SIM, BOARD], connected);
    assert.equal(rows[0]!.state, "connected");
    assert.equal(rows[0]!.connect, null);
    assert.equal(rows[1]!.state, "available");
});

test("a lost drone keeps its row instead of vanishing", () => {
    const connected: Connection = {
        via: "udp",
        id: "firmware",
        kind: 1,
        kindName: "firmware",
        live: false,
    };
    const rows = candidateRows([], connected);
    assert.equal(rows.length, 1);
    assert.equal(rows[0]!.label, "firmware");
    assert.equal(rows[0]!.state, "lost");
    assert.equal(rows[0]!.connect, null);
});
