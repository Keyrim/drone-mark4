import assert from "node:assert/strict";
import { test } from "node:test";

import { LogLevel, NodeKind } from "../src/gen/mark4_pb";
import {
    buildLevelTree,
    diffLevelTree,
    everyTarget,
    type LevelItem,
    type LevelNode,
    planLevelChange,
} from "../src/logTree";

const sim: LevelNode = {
    id: 0xd5000001,
    kind: NodeKind.DRONE_SIM,
    kindName: "drone_sim",
    name: "sim-a",
    logModules: [
        { id: 1, name: "platform/imu", level: LogLevel.INFO },
        { id: 2, name: "platform/baro", level: LogLevel.DEBUG },
        { id: 3, name: "rc", level: LogLevel.INFO },
    ],
};

const hub: LevelNode = {
    id: 0x0000000a,
    kind: NodeKind.GATEWAY,
    kindName: "gateway",
    name: "hub-bench",
    logModules: [
        { id: 1, name: "platform/imu", level: LogLevel.INFO },
        { id: 7, name: "gateway/ws", level: LogLevel.INFO },
    ],
};

const find = (items: LevelItem[], label: string): LevelItem => {
    const found = items.find((item) => item.label === label);
    assert.ok(found, `no item labelled ${label} in [${items.map((item) => item.label).join(", ")}]`);
    return found;
};

test("by node, modules sit under their prefix and show their level", () => {
    const roots = buildLevelTree([sim], "byNode");
    assert.equal(roots.length, 1);
    const node = roots[0] as LevelItem;
    assert.equal(node.label, "sim-a");
    assert.equal(node.description, "drone_sim d5000001");
    assert.equal(node.targets.length, 3, "a set on the node moves every module");
    const folder = find(node.children, "platform/");
    assert.deepEqual(
        folder.children.map((child) => child.label),
        ["baro", "imu"],
    );
    assert.equal(find(folder.children, "imu").description, "INFO");
    assert.equal(find(folder.children, "baro").description, "DEBUG");
    assert.deepEqual(folder.targets, [
        { nodeId: sim.id, moduleId: 2 },
        { nodeId: sim.id, moduleId: 1 },
    ]);
    // A module with no prefix, and a lone prefix, stay flat
    assert.equal(find(node.children, "rc").targets.length, 1);
});

test("a prefix worn by a single module does not become a folder", () => {
    const roots = buildLevelTree([hub], "byNode");
    const node = roots[0] as LevelItem;
    assert.deepEqual(
        node.children.map((child) => child.label).sort(),
        ["gateway/ws", "platform/imu"],
    );
});

test("by module, a name lists the nodes that have it", () => {
    const roots = buildLevelTree([sim, hub], "byModule");
    const folder = find(roots, "platform/");
    const imu = find(folder.children, "imu");
    assert.deepEqual(
        imu.children.map((child) => child.label),
        ["sim-a", "hub-bench"],
    );
    assert.equal(imu.description, "INFO", "both nodes agree");
    assert.deepEqual(imu.targets, [
        { nodeId: sim.id, moduleId: 1 },
        { nodeId: hub.id, moduleId: 1 },
    ]);
    assert.equal(imu.children[1]?.description, "gateway 0000000a INFO");
    assert.equal(find(folder.children, "baro").description, "DEBUG");
    assert.equal(folder.targets.length, 3, "the prefix covers both nodes");
});

test("a module the nodes disagree on reads as mixed", () => {
    const other: LevelNode = { ...hub, logModules: [{ id: 1, name: "platform/imu", level: LogLevel.TRACE }] };
    const roots = buildLevelTree([sim, other], "byModule");
    assert.equal(find(find(roots, "platform/").children, "imu").description, "mixed");
});

test("a node with no module table is not listed", () => {
    assert.deepEqual(buildLevelTree([{ ...sim, logModules: [] }], "byNode"), []);
    assert.deepEqual(buildLevelTree([{ ...sim, logModules: [] }], "byModule"), []);
});

test("the same table twice asks for no redraw at all", () => {
    for (const mode of ["byNode", "byModule"] as const) {
        const changes = diffLevelTree(buildLevelTree([sim, hub], mode), buildLevelTree([sim, hub], mode));
        assert.deepEqual(changes, { structural: false, changed: [] }, mode);
    }
});

test("a level that moved redraws the root that holds it, in both modes", () => {
    const moved: LevelNode = {
        ...sim,
        logModules: sim.logModules.map((module) => (module.id === 3 ? { ...module, level: LogLevel.WARN } : module)),
    };
    const byNode = diffLevelTree(buildLevelTree([sim, hub], "byNode"), buildLevelTree([moved, hub], "byNode"));
    assert.equal(byNode.structural, false);
    assert.deepEqual(byNode.changed, ["node:d5000001"], "the node of the module, not the other one");

    const byModule = diffLevelTree(buildLevelTree([sim, hub], "byModule"), buildLevelTree([moved, hub], "byModule"));
    assert.equal(byModule.structural, false);
    assert.deepEqual(byModule.changed, ["module:rc"], "the module the node moved, not the other names");
});

test("a node appearing or leaving is a whole redraw", () => {
    const before = buildLevelTree([sim], "byNode");
    assert.deepEqual(diffLevelTree(before, buildLevelTree([sim, hub], "byNode")), {
        structural: true,
        changed: [],
    });
    assert.deepEqual(diffLevelTree(buildLevelTree([sim, hub], "byNode"), before), {
        structural: true,
        changed: [],
    });
});

test("a node renamed redraws its root and nothing else", () => {
    const renamed = { ...sim, name: "sim-b" };
    const changes = diffLevelTree(buildLevelTree([sim, hub], "byNode"), buildLevelTree([renamed, hub], "byNode"));
    assert.deepEqual(changes.changed, ["node:d5000001"]);
});

test("one gesture moves the nodes and the view over the same scope", () => {
    const node = buildLevelTree([sim], "byNode")[0] as LevelItem;
    const plan = planLevelChange(node.targets, LogLevel.DEBUG);
    assert.equal(plan.level, LogLevel.DEBUG);
    assert.deepEqual(plan.control, node.targets, "one set per module below the item");
    assert.deepEqual(plan.display, node.targets, "the view follows the same scope");
    assert.deepEqual(plan.queryNodes, [sim.id], "one query per node, whatever the module count");
    assert.deepEqual(planLevelChange([], LogLevel.INFO).queryNodes, []);
});

test("everything means every module of every node that published one", () => {
    const quiet: LevelNode = {
        id: 0x00000042,
        kind: NodeKind.FIRMWARE,
        kindName: "firmware",
        name: "board",
        logModules: [],
    };
    const scope = everyTarget([sim, quiet, hub]);
    assert.deepEqual(scope.skipped, [quiet.id], "a node with no table is named, not guessed at");
    assert.deepEqual(scope.targets, [
        { nodeId: sim.id, moduleId: 1 },
        { nodeId: sim.id, moduleId: 2 },
        { nodeId: sim.id, moduleId: 3 },
        { nodeId: hub.id, moduleId: 1 },
        { nodeId: hub.id, moduleId: 7 },
    ]);
    assert.deepEqual(planLevelChange(scope.targets, LogLevel.TRACE).queryNodes, [sim.id, hub.id]);
    assert.deepEqual(everyTarget([]), { targets: [], skipped: [] });
});
