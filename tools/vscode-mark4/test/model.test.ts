import { create } from "@bufbuild/protobuf";
import assert from "node:assert/strict";
import { test } from "node:test";

import { NodeSchema } from "../src/gen/gateway_pb";
import { AnnounceSchema, NodeKind } from "../src/gen/mark4_pb";
import { diffNodeRows, hexNodeId, LIVE_MS, nodeRows, simInstance, simNodeId, wireMismatch } from "../src/model";

const announce = (fields: Partial<{ kind: NodeKind; name: string; wireHash: number; buildEpoch: number; gitHash: string }>) =>
    create(AnnounceSchema, fields);

const node = (fields: Parameters<typeof create<typeof NodeSchema>>[1]) => create(NodeSchema, fields);

test("a node id prints as 8 hex digits, high bit included", () => {
    assert.equal(hexNodeId(1), "00000001");
    assert.equal(hexNodeId(0xd5000002), "d5000002");
    assert.equal(hexNodeId(-1), "ffffffff");
});

test("the drone_sim instance is what is left of its node id", () => {
    assert.equal(simInstance(simNodeId(3)), 3);
    assert.equal(simInstance(0x12345678), undefined);
    assert.equal(simInstance(simNodeId(0)), undefined, "instance 0 is not one of ours");
});

test("a wire mismatch needs both hashes to be known", () => {
    assert.equal(wireMismatch(undefined, 7), false);
    assert.equal(wireMismatch(announce({ wireHash: 8 }), 0), false);
    assert.equal(wireMismatch(announce({ wireHash: 8 }), 7), true);
    assert.equal(wireMismatch(announce({ wireHash: 7 }), 7), false);
});

test("a row carries the kind, its icon and the identity of the node", () => {
    const [row] = nodeRows(
        [
            node({
                id: 0xd5000001,
                address: "127.0.0.1",
                port: 47820,
                lastSeenMsAgo: 120,
                received: 42,
                announce: announce({
                    kind: NodeKind.DRONE_SIM,
                    name: "sim-a",
                    wireHash: 7,
                    buildEpoch: 1_700_000_000,
                    gitHash: "abcdef12",
                }),
            }),
        ],
        7,
    );
    assert.ok(row);
    assert.equal(row.name, "sim-a");
    assert.equal(row.kindName, "drone_sim");
    assert.equal(row.icon, "vm");
    assert.equal(row.hex, "d5000001");
    assert.equal(row.live, true);
    assert.equal(row.mismatch, false);
    assert.match(row.tooltip, /127\.0\.0\.1:47820/);
    assert.match(row.tooltip, /built 2023-11-14/);
    assert.match(row.tooltip, /git abcdef12/);
    assert.match(row.tooltip, /received 42/);
});

test("an unannounced node and an unknown kind stay generic", () => {
    const rows = nodeRows([node({ id: 5 }), node({ id: 6, announce: announce({ kind: 99 as NodeKind }) })], 0);
    assert.equal(rows[0]?.name, "unknown");
    assert.equal(rows[0]?.icon, "question");
    assert.match(rows[0]?.tooltip ?? "", /no announce yet/);
    assert.equal(rows[1]?.kindName, "kind 99");
    assert.equal(rows[1]?.icon, "question");
});

test("the relay of the ESP32 is named even before the schema knows it", () => {
    const [row] = nodeRows([node({ id: 7, announce: announce({ kind: 6 as NodeKind }) })], 0);
    assert.equal(row?.kindName, "relay");
    assert.equal(row?.icon, "radio-tower");
});

test("a node fades once its last frame is old, and a mismatch is flagged", () => {
    const rows = nodeRows(
        [
            node({ id: 1, lastSeenMsAgo: LIVE_MS - 1, announce: announce({ kind: NodeKind.GATEWAY, wireHash: 7 }) }),
            node({ id: 2, lastSeenMsAgo: LIVE_MS, announce: announce({ kind: NodeKind.FIRMWARE, wireHash: 9 }) }),
        ],
        7,
    );
    assert.equal(rows[0]?.live, true);
    assert.equal(rows[1]?.live, false);
    assert.equal(rows[1]?.mismatch, true);
    assert.match(rows[1]?.tooltip ?? "", /WIRE MISMATCH/);
});

const table = (fields: { lastSeenMsAgo?: number; received?: number; name?: string; wireHash?: number } = {}) =>
    nodeRows(
        [
            node({
                id: 0xd5000001,
                lastSeenMsAgo: fields.lastSeenMsAgo ?? 100,
                received: fields.received ?? 10,
                announce: announce({
                    kind: NodeKind.DRONE_SIM,
                    name: fields.name ?? "sim-a",
                    wireHash: fields.wireHash ?? 7,
                }),
            }),
            node({ id: 0x0000000a, announce: announce({ kind: NodeKind.GATEWAY, wireHash: 7 }) }),
        ],
        7,
    );

test("counters and a last-seen that moves do not redraw a line", () => {
    const changes = diffNodeRows(table(), table({ received: 4321, lastSeenMsAgo: 900 }));
    assert.deepEqual(changes, { structural: false, changed: [] });
});

test("a node crossing the fading threshold redraws that line alone", () => {
    const changes = diffNodeRows(table(), table({ lastSeenMsAgo: LIVE_MS }));
    assert.deepEqual(changes, { structural: false, changed: [0xd5000001] });
});

test("a name and a wire mismatch redraw the line they belong to", () => {
    assert.deepEqual(diffNodeRows(table(), table({ name: "sim-b" })).changed, [0xd5000001]);
    assert.deepEqual(diffNodeRows(table(), table({ wireHash: 9 })).changed, [0xd5000001]);
});

test("a node appearing, leaving or moving is a whole redraw", () => {
    const one = table().slice(0, 1);
    assert.equal(diffNodeRows(one, table()).structural, true);
    assert.equal(diffNodeRows(table(), one).structural, true);
    assert.equal(diffNodeRows(table(), [...table()].reverse()).structural, true);
});
