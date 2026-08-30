import assert from "node:assert/strict";
import test from "node:test";

import { fromBinary, toBinary } from "@bufbuild/protobuf";

import { EnvelopeSchema, RcMode } from "../src/gen/mark4_pb";
import { MODE_ALTITUDE_AUTO, MODE_MANUAL, SAFE_RC, TICK_MS, clamp01, rcEnvelope } from "../src/console/rc";

test("the safe state is cut, disarmed, manual, stick down", () => {
    assert.equal(SAFE_RC.kill, true);
    assert.equal(SAFE_RC.arm, false);
    assert.equal(SAFE_RC.mode, MODE_MANUAL);
    assert.equal(SAFE_RC.throttle, 0);
    assert.equal(MODE_MANUAL, RcMode.RC_MANUAL);
    assert.equal(MODE_ALTITUDE_AUTO, RcMode.RC_ALTITUDE_AUTO);
});

test("the stream period is 10 Hz", () => {
    assert.equal(TICK_MS, 100);
});

test("clamp01 keeps the throttle inside [0, 1] and turns NaN into 0", () => {
    assert.equal(clamp01(-0.5), 0);
    assert.equal(clamp01(0.5), 0.5);
    assert.equal(clamp01(7), 1);
    assert.equal(clamp01(Number.NaN), 0);
});

test("the Rc envelope carries exactly the four fields, throttle clamped", () => {
    const envelope = rcEnvelope({ kill: false, arm: true, mode: MODE_ALTITUDE_AUTO, throttle: 1.5 });
    const back = fromBinary(EnvelopeSchema, toBinary(EnvelopeSchema, envelope));
    assert.equal(back.body.case, "rc");
    if (back.body.case === "rc") {
        assert.equal(back.body.value.kill, false);
        assert.equal(back.body.value.arm, true);
        assert.equal(back.body.value.mode, RcMode.RC_ALTITUDE_AUTO);
        assert.equal(back.body.value.throttle, 1);
    }
});
