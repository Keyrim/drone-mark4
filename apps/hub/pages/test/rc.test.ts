import assert from "node:assert/strict";
import test from "node:test";

import { MODE_ALTITUDE_AUTO, MODE_MANUAL, SAFE_RC, clamp01, rcPayload } from "../src/console/rc";

test("the safe state is cut, disarmed, stick down, manual", () => {
    assert.equal(SAFE_RC.kill, true);
    assert.equal(SAFE_RC.arm, false);
    assert.equal(SAFE_RC.throttle, 0);
    assert.equal(SAFE_RC.mode, MODE_MANUAL);
});

test("the throttle is clamped whatever the input element reports", () => {
    assert.equal(clamp01(-0.5), 0);
    assert.equal(clamp01(0.42), 0.42);
    assert.equal(clamp01(7), 1);
    assert.equal(clamp01(Number.NaN), 0);
});

test("the payload is exactly what the hub expects", () => {
    const payload = rcPayload(
        { kill: false, arm: true, mode: MODE_ALTITUDE_AUTO, throttle: 0.5 },
        "drone_sim"
    );
    assert.deepEqual(payload, {
        type: "rc",
        target: "drone_sim",
        kill: 0,
        arm: 1,
        mode: MODE_ALTITUDE_AUTO,
        throttle: 0.5,
    });
});

test("a payload built from the safe state commands a cut", () => {
    const payload = rcPayload(SAFE_RC, "firmware");
    assert.equal(payload["kill"], 1);
    assert.equal(payload["arm"], 0);
    assert.equal(payload["throttle"], 0);
});
