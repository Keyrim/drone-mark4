import assert from "node:assert/strict";
import test from "node:test";

import { isUsable, rotateVec, toRenderQuat } from "../src/attitude/remap";
import type { Quat } from "../src/shared/quat";

/*
 * The remap is checked the only way that catches a wrong handedness: rotate a
 * body axis by the remapped attitude and look at where it lands in the render
 * frame. Body axes in render coordinates: the nose (drone +x) is render -z,
 * the mast (drone +z) is render +y, the left arm (drone +y) is render -x.
 */
const NOSE: [number, number, number] = [0, 0, -1];
const MAST: [number, number, number] = [0, 1, 0];

const HALF = Math.SQRT1_2;

function assertVec(actual: number[], expected: number[]): void {
    for (let i = 0; i < expected.length; i++) {
        assert.ok(
            Math.abs((actual[i] as number) - (expected[i] as number)) < 1e-12,
            `component ${i}: ${actual[i]} != ${expected[i]}`
        );
    }
}

/** Rotates a body direction by the remapped attitude, in render coordinates. */
function point(q: Quat, body: [number, number, number]): [number, number, number] {
    return rotateVec(toRenderQuat(q), body);
}

test("identity attitude leaves the nose forward and the mast up", () => {
    const identity: Quat = [1, 0, 0, 0];
    assertVec(point(identity, NOSE), [0, 0, -1]);
    assertVec(point(identity, MAST), [0, 1, 0]);
});

test("a +90 deg yaw in the drone frame turns the nose to the left", () => {
    // Rotation about the drone z axis (up): x forward goes to y left, which
    // is -x in the render frame. The mast stays up.
    const yaw90: Quat = [HALF, 0, 0, HALF];
    assertVec(point(yaw90, NOSE), [-1, 0, 0]);
    assertVec(point(yaw90, MAST), [0, 1, 0]);
});

test("a +90 deg roll in the drone frame tips the mast to the right", () => {
    // Rotation about the drone x axis (forward): z up goes to -y, the right
    // hand side, which is +x in the render frame. The nose stays forward.
    const roll90: Quat = [HALF, HALF, 0, 0];
    assertVec(point(roll90, NOSE), [0, 0, -1]);
    assertVec(point(roll90, MAST), [1, 0, 0]);
});

test("a +90 deg pitch in the drone frame points the nose down", () => {
    // Rotation about the drone y axis (left): x forward goes to -z, down,
    // which is -y in the render frame. The mast goes forward.
    const pitch90: Quat = [HALF, 0, HALF, 0];
    assertVec(point(pitch90, NOSE), [0, -1, 0]);
    assertVec(point(pitch90, MAST), [0, 0, -1]);
});

test("the remap keeps the quaternion normalized", () => {
    const q: Quat = [2, 0, 0, 0];
    assertVec([...toRenderQuat(q)], [1, 0, 0, 0]);
});

test("torn and non-finite quaternions are rejected", () => {
    assert.equal(isUsable([1, 0, 0, 0]), true);
    assert.equal(isUsable([0.6, 0.5, 0.4, 0.3]), true);
    assert.equal(isUsable([0, 0, 0, 0]), false);
    assert.equal(isUsable([0.5, 0, 0, 0]), false);
    assert.equal(isUsable([Number.NaN, 0, 0, 0]), false);
    assert.equal(isUsable([1, Number.POSITIVE_INFINITY, 0, 0]), false);
});
