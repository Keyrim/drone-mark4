import assert from "node:assert/strict";
import test from "node:test";

import { TelemetryUnit } from "../src/gen/mark4_pb";
import { type Descriptor } from "../src/telemetry/model";
import { ancestorsOf, buildTree, leavesOf, type TreeNode } from "../src/telemetry/tree";

function d(id: number, name: string, unit = TelemetryUnit.UNITLESS): Descriptor {
    return { id, name, unit };
}

const TABLE: Descriptor[] = [
    d(0, "estimator/attitude/pitch", TelemetryUnit.RAD),
    d(1, "estimator/attitude/roll", TelemetryUnit.RAD),
    d(2, "estimator/attitude/w"),
    d(3, "estimator/altitude", TelemetryUnit.M),
    d(4, "rc/throttle"),
    d(5, "sim/truth/position_x", TelemetryUnit.M),
    d(6, "sim/truth/position_y", TelemetryUnit.M),
    d(7, "throw/count", TelemetryUnit.COUNT),
    d(8, "throw/state"),
];

function labels(nodes: TreeNode[]): string[] {
    return nodes.map((node) => node.label);
}

test("names fold into folders on their slashes, folders first", () => {
    const tree = buildTree(TABLE);
    assert.deepEqual(labels(tree), ["estimator", "sim/truth", "throw", "rc/throttle"]);
    const estimator = tree[0] as TreeNode;
    assert.equal(estimator.path, "estimator");
    // The folder `attitude` sorts before the leaf `altitude`: folders first.
    assert.deepEqual(labels(estimator.children), ["attitude", "altitude"]);
    const attitude = estimator.children[0] as TreeNode;
    assert.deepEqual(labels(attitude.children), ["pitch", "roll", "w"]);
    assert.equal(attitude.children[0]?.leaf?.name, "estimator/attitude/pitch");
});

test("a folder of one child is not a click: it is merged into its parent", () => {
    const tree = buildTree(TABLE);
    // `rc` holds one measure: the leaf is lifted with its path as label.
    const throttle = tree[3] as TreeNode;
    assert.equal(throttle.leaf?.name, "rc/throttle");
    assert.equal(throttle.children.length, 0);
    // `sim` holds one folder: the two fold into one row keeping the path.
    const truth = tree[1] as TreeNode;
    assert.equal(truth.path, "sim/truth");
    assert.deepEqual(labels(truth.children), ["position_x", "position_y"]);
});

test("the leaves of a row are every measure under it", () => {
    const tree = buildTree(TABLE);
    assert.deepEqual(
        leavesOf(tree[0] as TreeNode).map((leaf) => leaf.id),
        [0, 1, 2, 3]
    );
    assert.deepEqual(leavesOf(tree[3] as TreeNode).map((leaf) => leaf.id), [4]);
});

test("a filter keeps the measures whose name contains it", () => {
    const tree = buildTree(TABLE, "position_x");
    assert.deepEqual(labels(tree), ["sim/truth/position_x"]);
    assert.equal(tree[0]?.leaf?.id, 5);
    assert.deepEqual(labels(buildTree(TABLE, "attitude")), ["estimator/attitude"]);
    assert.deepEqual(buildTree(TABLE, "nothing"), []);
});

test("the ancestors of a measure are its folders, outermost first", () => {
    assert.deepEqual(ancestorsOf("estimator/attitude/pitch"), ["estimator", "estimator/attitude"]);
    assert.deepEqual(ancestorsOf("flat"), []);
});
