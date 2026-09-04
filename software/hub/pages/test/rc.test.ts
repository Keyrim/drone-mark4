import assert from "node:assert/strict";
import test from "node:test";

import { fromBinary, toBinary } from "@bufbuild/protobuf";

import { EnvelopeSchema, RcMode } from "../src/gen/mark4_pb";
import {
    MODE_ALTITUDE_AUTO,
    MODE_LEVEL,
    MODE_MANUAL,
    MODE_OPTIONS,
    SAFE_RC,
    TICK_MS,
    clamp01,
    clampAxis,
    rcEnvelope,
} from "../src/console/rc";

test("the safe state is cut, disarmed, manual, stick down, sticks released", () => {
    assert.equal(SAFE_RC.kill, true);
    assert.equal(SAFE_RC.arm, false);
    assert.equal(SAFE_RC.mode, MODE_MANUAL);
    assert.equal(SAFE_RC.throttle, 0);
    assert.equal(SAFE_RC.roll, 0);
    assert.equal(SAFE_RC.pitch, 0);
    assert.equal(SAFE_RC.yaw, 0);
    assert.equal(MODE_MANUAL, RcMode.RC_MANUAL);
    assert.equal(MODE_ALTITUDE_AUTO, RcMode.RC_ALTITUDE_AUTO);
    assert.equal(MODE_LEVEL, RcMode.RC_LEVEL);
});

test("the stream period is 20 Hz, four messages inside the 200 ms fail-safe", () => {
    assert.equal(TICK_MS, 50);
    assert.ok(4 * TICK_MS <= 200);
});

test("the selector lists every mode once, level first", () => {
    assert.equal(MODE_OPTIONS[0]?.mode, MODE_LEVEL);
    const modes = MODE_OPTIONS.map((option) => option.mode).sort();
    assert.deepEqual(modes, [MODE_MANUAL, MODE_ALTITUDE_AUTO, MODE_LEVEL].sort());
});

test("clamp01 keeps the throttle inside [0, 1] and turns NaN into 0", () => {
    assert.equal(clamp01(-0.5), 0);
    assert.equal(clamp01(0.5), 0.5);
    assert.equal(clamp01(7), 1);
    assert.equal(clamp01(Number.NaN), 0);
});

test("clampAxis keeps a stick inside [-1, 1] and turns NaN into released", () => {
    assert.equal(clampAxis(-3), -1);
    assert.equal(clampAxis(-0.25), -0.25);
    assert.equal(clampAxis(0.75), 0.75);
    assert.equal(clampAxis(2), 1);
    assert.equal(clampAxis(Number.NaN), 0);
});

test("the Rc envelope carries the switches, the mode, the throttle and the three sticks, clamped", () => {
    const envelope = rcEnvelope({
        kill: false,
        arm: true,
        mode: MODE_LEVEL,
        throttle: 1.5,
        roll: -2,
        pitch: 0.5,
        yaw: 3,
    });
    const back = fromBinary(EnvelopeSchema, toBinary(EnvelopeSchema, envelope));
    assert.equal(back.body.case, "rc");
    if (back.body.case === "rc") {
        assert.equal(back.body.value.kill, false);
        assert.equal(back.body.value.arm, true);
        assert.equal(back.body.value.mode, RcMode.RC_LEVEL);
        assert.equal(back.body.value.throttle, 1);
        assert.equal(back.body.value.roll, -1);
        assert.equal(back.body.value.pitch, 0.5);
        assert.equal(back.body.value.yaw, 1);
    }
});
