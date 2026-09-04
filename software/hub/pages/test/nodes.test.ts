import assert from "node:assert/strict";
import test from "node:test";

import { create } from "@bufbuild/protobuf";

import { NodeTableSchema } from "../src/gen/gateway_pb";
import { NodeKind } from "../src/gen/mark4_pb";
import {
    FADING_MS,
    NodeModel,
    diffNodes,
    hexNodeId,
    isDrone,
    isUpdatable,
    nodeColor,
    nodeLabel,
    wireMismatch,
    type NodeView,
} from "../src/shared/nodes";

const GATEWAY_HASH = 0x7e8201a9;

/** A table with the gateway, a plant and two drone_sim, as the gateway sends it. */
function table(extra: { id: number; kind: NodeKind; wireHash?: number }[] = []) {
    return create(NodeTableSchema, {
        nodes: [
            { id: 1, lastSeenMsAgo: 0, announce: { kind: NodeKind.GATEWAY, name: "hub-bench", wireHash: GATEWAY_HASH } },
            { id: 2, address: "192.168.1.5", port: 4711, lastSeenMsAgo: 300, announce: { kind: NodeKind.PLANT, wireHash: GATEWAY_HASH } },
            { id: 10, lastSeenMsAgo: 20, announce: { kind: NodeKind.DRONE_SIM, name: "sim-a", wireHash: GATEWAY_HASH } },
            { id: 11, lastSeenMsAgo: 40, announce: { kind: NodeKind.DRONE_SIM, name: "sim-b", wireHash: GATEWAY_HASH } },
            ...extra.map((node) => ({
                id: node.id,
                announce: { kind: node.kind, wireHash: node.wireHash ?? GATEWAY_HASH },
            })),
        ],
    });
}

test("the node table lists every node by id and the drones apart", () => {
    const model = new NodeModel();
    model.setGatewayWireHash(GATEWAY_HASH);
    model.applyTable(table());
    assert.equal(model.list().length, 4);
    assert.deepEqual(model.drones().map((node) => node.id), [10, 11]);
    const plant = model.get(2)!;
    assert.equal(plant.kindName, "plant");
    assert.equal(plant.name, "plant");
    assert.equal(plant.address, "192.168.1.5");
    assert.equal(model.get(10)!.name, "sim-a");
    assert.equal(isDrone(model.get(1)!), false);
});

test("the update panel targets the drones and the relay, nothing else", () => {
    const model = new NodeModel();
    model.applyTable(table([{ id: 13, kind: NodeKind.RELAY }]));
    assert.deepEqual(model.list().filter(isUpdatable).map((node) => node.id).sort(), [10, 11, 13]);
    assert.equal(isDrone(model.get(13)!), false);
});

test("two drone_sim are two nodes, never one flapping entry", () => {
    const model = new NodeModel();
    const seen: number[][] = [];
    model.onChange((nodes) => seen.push(nodes.filter(isDrone).map((node) => node.id).sort()));
    model.applyTable(table());
    model.applyTable(table());
    model.applyTable(table());
    assert.deepEqual(seen, [[10, 11], [10, 11], [10, 11]]);
});

test("a widget is created when a drone appears and destroyed when it leaves", () => {
    const model = new NodeModel();
    // The console's rule: a widget per drone, destroyed when its id leaves
    const widgets = new Set<number>();
    const events: string[] = [];
    model.onChange((_nodes, diff) => {
        for (const id of diff.removed) {
            if (widgets.delete(id)) {
                events.push(`destroy ${id}`);
            }
        }
        for (const node of diff.added) {
            if (isDrone(node)) {
                widgets.add(node.id);
                events.push(`create ${node.id}`);
            }
        }
    });
    model.applyTable(table());
    model.applyTable(table([{ id: 12, kind: NodeKind.FIRMWARE }]));
    model.applyTable(create(NodeTableSchema, { nodes: table().nodes.filter((node) => node.id !== 11) }));
    model.clear();
    assert.deepEqual(events, ["create 10", "create 11", "create 12", "destroy 11", "destroy 12", "destroy 10"]);
    assert.equal(widgets.size, 0);
});

test("a frame from an unlisted node adds a placeholder; its announce later turns it into a drone", () => {
    const model = new NodeModel();
    const events: string[] = [];
    model.onChange((_nodes, diff) => {
        events.push(...diff.removed.map((id) => `-${id}`), ...diff.added.map((node) => `+${node.id}:${node.kindName}`));
    });
    model.noteFrame(42);
    assert.equal(model.get(42)!.kindName, "unknown");
    assert.equal(isDrone(model.get(42)!), false);
    model.applyTable(table([{ id: 42, kind: NodeKind.FIRMWARE }]));
    assert.equal(isDrone(model.get(42)!), true);
    // The kind change is a remove plus an add: the widget is rebuilt for the right nature
    assert.ok(events.includes("+42:unknown"));
    assert.ok(events.includes("-42"));
    assert.ok(events.includes("+42:firmware"));
});

test("a frame refreshes the age of a listed node without an event", () => {
    const model = new NodeModel();
    model.applyTable(table());
    let changes = 0;
    model.onChange(() => changes++);
    assert.equal(model.get(2)!.ageMs, 300);
    model.noteFrame(2);
    assert.equal(model.get(2)!.ageMs, 0);
    assert.equal(changes, 0);
});

test("the wire mismatch flag compares the announce with the gateway's hash", () => {
    const model = new NodeModel();
    model.applyTable(table([{ id: 13, kind: NodeKind.FIRMWARE, wireHash: 0x11111111 }]));
    // No status yet: nothing is flagged, the reference is unknown
    assert.equal(model.get(13)!.wireMismatch, false);
    model.setGatewayWireHash(GATEWAY_HASH);
    assert.equal(model.get(13)!.wireMismatch, true);
    assert.equal(model.get(10)!.wireMismatch, false);
    assert.equal(wireMismatch(undefined, GATEWAY_HASH), false);
    assert.equal(wireMismatch(model.get(13)!.announce, 0), false);
});

test("diffNodes reports kind changes on both sides and nothing for an identical table", () => {
    const a: NodeView = {
        id: 1, kind: NodeKind.DRONE_SIM, kindName: "drone_sim", name: "a", address: "", ageMs: 0,
        received: 0, lost: 0, wireMismatch: false, announce: undefined, logModules: [],
    };
    const before = new Map([[1, a]]);
    assert.deepEqual(diffNodes(before, new Map([[1, { ...a, ageMs: 5 }]])), { added: [], removed: [] });
    const changed = diffNodes(before, new Map([[1, { ...a, kind: NodeKind.FIRMWARE }]]));
    assert.equal(changed.added.length, 1);
    assert.deepEqual(changed.removed, [1]);
});

test("a node color is stable and comes from the palette", () => {
    assert.equal(nodeColor(0x51300001), nodeColor(0x51300001));
    assert.match(nodeColor(7), /^#[0-9a-f]{6}$/);
});

test("a node reads as its name plus its id in 8 hex digits", () => {
    assert.equal(hexNodeId(0x51300001), "51300001");
    assert.equal(hexNodeId(0xffffffff), "ffffffff");
    const model = new NodeModel();
    model.applyTable(table([{ id: 0x51300001, kind: NodeKind.FIRMWARE }]));
    assert.equal(nodeLabel(model.get(10)!), "sim-a 0000000a");
    // An unnamed node falls back to its kind, so a label is never just an id
    assert.equal(nodeLabel(model.get(0x51300001)!), "firmware 51300001");
});

test("a widget header names the drone, badges its kind and shows its id", () => {
    const model = new NodeModel();
    model.setGatewayWireHash(GATEWAY_HASH);
    model.applyTable(table([{ id: 12, kind: NodeKind.FIRMWARE, wireHash: 0x11111111 }]));
    // What the header draws: title, badge, muted id, and the wire flag
    const header = (node: NodeView) => [node.name, node.kindName, hexNodeId(node.id), node.wireMismatch];
    assert.deepEqual(header(model.get(10)!), ["sim-a", "drone_sim", "0000000a", false]);
    assert.deepEqual(header(model.get(12)!), ["firmware", "firmware", "0000000c", true]);
});

test("a widget fades on the age the table reports, and a frame clears it", () => {
    const model = new NodeModel();
    model.applyTable(
        create(NodeTableSchema, {
            nodes: [
                { id: 10, lastSeenMsAgo: FADING_MS - 1, announce: { kind: NodeKind.DRONE_SIM, name: "sim-a" } },
                { id: 11, lastSeenMsAgo: FADING_MS, announce: { kind: NodeKind.DRONE_SIM, name: "sim-b" } },
            ],
        })
    );
    const fading = (id: number) => model.get(id)!.ageMs >= FADING_MS;
    assert.equal(fading(10), false);
    assert.equal(fading(11), true);
    model.noteFrame(11);
    assert.equal(fading(11), false);
});

test("the source selector lists the drones only, in a stable order", () => {
    const model = new NodeModel();
    model.applyTable(table([{ id: 3, kind: NodeKind.BATCH }, { id: 12, kind: NodeKind.FIRMWARE }]));
    // The plots selector and the update panel share this list
    assert.deepEqual(model.drones().map(nodeLabel), ["sim-a 0000000a", "sim-b 0000000b", "firmware 0000000c"]);
});
