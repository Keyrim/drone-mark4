import assert from "node:assert/strict";
import test from "node:test";

import {
    DEADMAN_MS,
    LATE_TICK_MS,
    MODE_ALTITUDE_AUTO,
    RAMP_FINE_PER_S,
    RAMP_PER_S,
    SAFE_RC,
    keyEvent,
    rcPayload,
    rcReduce,
    type RcEvent,
    type RcState,
} from "../src/console/rc";

/** Applies a run of events in order, starting from the safe state. */
function run(...events: RcEvent[]): RcState {
    return events.reduce(rcReduce, SAFE_RC);
}

const engage: RcEvent = { type: "engage", atMs: 0 };

test("the safe state is cut, disarmed and streaming nothing", () => {
    assert.equal(SAFE_RC.kill, true);
    assert.equal(SAFE_RC.arm, false);
    assert.equal(SAFE_RC.throttle, 0);
    assert.equal(SAFE_RC.engaged, false);
});

test("a panic kill is never an un-kill", () => {
    const flying = run(engage, { type: "toggleKill", atMs: 10 });
    assert.equal(flying.kill, false);
    const panicked = rcReduce(flying, { type: "panic" });
    assert.equal(panicked.kill, true);
    assert.equal(panicked.throttle, 0);
    // A second panic cannot toggle it back
    assert.equal(rcReduce(panicked, { type: "panic" }).kill, true);
    // Nor can one while disengaged
    assert.equal(rcReduce(SAFE_RC, { type: "panic" }).kill, true);
});

test("the buttons are dead while the pilot toggle is off", () => {
    assert.deepEqual(rcReduce(SAFE_RC, { type: "toggleArm", atMs: 5 }), SAFE_RC);
    assert.deepEqual(rcReduce(SAFE_RC, { type: "toggleKill", atMs: 5 }), SAFE_RC);
    assert.deepEqual(rcReduce(SAFE_RC, { type: "setThrottle", value: 1, atMs: 5 }), SAFE_RC);
    assert.deepEqual(rcReduce(SAFE_RC, { type: "tick", atMs: 5 }), SAFE_RC);
});

test("a held arrow ramps at the tick, at the coarse or the fine rate", () => {
    const up = run(engage, { type: "hold", key: "up", down: true, atMs: 0 });
    // One second of ticks, applied in one 100 ms step at a time
    let state = up;
    for (let ms = 100; ms <= 1000; ms += 100) {
        state = rcReduce(state, { type: "tick", atMs: ms });
    }
    assert.ok(Math.abs(state.throttle - RAMP_PER_S) < 1e-9);

    let fine = run(
        engage,
        { type: "hold", key: "up", down: true, atMs: 0 },
        { type: "hold", key: "fine", down: true, atMs: 0 }
    );
    for (let ms = 100; ms <= 1000; ms += 100) {
        fine = rcReduce(fine, { type: "tick", atMs: ms });
    }
    assert.ok(Math.abs(fine.throttle - RAMP_FINE_PER_S) < 1e-9);
});

test("the ramp is clamped and reverses on the other arrow", () => {
    let state = run(engage, { type: "hold", key: "up", down: true, atMs: 0 });
    for (let ms = 100; ms <= 5000; ms += 100) {
        state = rcReduce(state, { type: "tick", atMs: ms });
    }
    assert.equal(state.throttle, 1);
    state = rcReduce(state, { type: "hold", key: "up", down: false, atMs: 5000 });
    state = rcReduce(state, { type: "hold", key: "down", down: true, atMs: 5000 });
    for (let ms = 5100; ms <= 10000; ms += 100) {
        state = rcReduce(state, { type: "tick", atMs: ms });
    }
    assert.equal(state.throttle, 0);
});

test("an auto-repeat press is not an event", () => {
    assert.equal(keyEvent("ArrowUp", false, true, true, 0), null);
    assert.equal(keyEvent("KeyK", false, true, true, 0), null);
    // The release of a repeated key still counts: it is what stops the ramp
    assert.deepEqual(keyEvent("ArrowUp", false, false, false, 7), {
        type: "hold",
        key: "up",
        down: false,
        atMs: 7,
    });
});

test("a repeated hold does not ramp twice", () => {
    const once = run(engage, { type: "hold", key: "up", down: true, atMs: 0 });
    const twice = rcReduce(once, { type: "hold", key: "up", down: true, atMs: 0 });
    assert.equal(
        rcReduce(twice, { type: "tick", atMs: 100 }).throttle,
        rcReduce(once, { type: "tick", atMs: 100 }).throttle
    );
});

test("the keys map to the one state the pilot steers", () => {
    assert.deepEqual(keyEvent("Space", false, false, true, 1), { type: "panic" });
    assert.deepEqual(keyEvent("Escape", false, false, true, 1), {
        type: "disengage",
        reason: "escape",
    });
    assert.deepEqual(keyEvent("KeyC", false, false, true, 1), {
        type: "setThrottle",
        value: 0.5,
        atMs: 1,
    });
    assert.deepEqual(keyEvent("Home", false, false, true, 1), {
        type: "setThrottle",
        value: 0,
        atMs: 1,
    });
    assert.equal(keyEvent("KeyQ", false, false, true, 1), null);
});

test("disengaging resets everything but the mode preference", () => {
    const flying = run(
        engage,
        { type: "toggleMode", atMs: 1 },
        { type: "toggleKill", atMs: 2 },
        { type: "toggleArm", atMs: 3 },
        { type: "setThrottle", value: 0.8, atMs: 4 },
        { type: "hold", key: "up", down: true, atMs: 5 }
    );
    assert.equal(flying.throttle, 0.8);
    const off = rcReduce(flying, { type: "disengage", reason: "blur" });
    assert.equal(off.engaged, false);
    assert.equal(off.kill, true);
    assert.equal(off.arm, false);
    assert.equal(off.throttle, 0);
    assert.equal(off.held.size, 0);
    assert.equal(off.reason, "blur");
    assert.equal(off.mode, MODE_ALTITUDE_AUTO);
});

test("a tick that arrives too late disengages", () => {
    const late = rcReduce(run(engage), { type: "tick", atMs: LATE_TICK_MS + 1 });
    assert.equal(late.engaged, false);
    assert.ok(late.reason !== "");
    // On time is on time
    assert.equal(rcReduce(run(engage), { type: "tick", atMs: LATE_TICK_MS }).engaged, true);
});

test("the deadman disengages after a minute without input", () => {
    let state = run(engage);
    for (let ms = 100; ms < DEADMAN_MS; ms += 100) {
        state = rcReduce(state, { type: "tick", atMs: ms });
    }
    assert.equal(state.engaged, true);
    state = rcReduce(state, { type: "tick", atMs: DEADMAN_MS });
    assert.equal(state.engaged, false);
    assert.ok(state.reason.includes("deadman"));
});

test("a key press pushes the deadman back", () => {
    let state = run(engage);
    for (let ms = 100; ms <= DEADMAN_MS - 100; ms += 100) {
        state = rcReduce(state, { type: "tick", atMs: ms });
    }
    state = rcReduce(state, { type: "toggleArm", atMs: DEADMAN_MS - 100 });
    state = rcReduce(state, { type: "tick", atMs: DEADMAN_MS });
    assert.equal(state.engaged, true);
});

test("the payload is what the hub reads, integers and all", () => {
    const flying = run(engage, { type: "toggleKill", atMs: 1 }, { type: "toggleArm", atMs: 2 });
    assert.deepEqual(rcPayload(flying, "drone_sim"), {
        type: "rc",
        target: "drone_sim",
        kill: 0,
        arm: 1,
        mode: 0,
        throttle: 0,
    });
    assert.equal(rcPayload(SAFE_RC, "firmware")["kill"], 1);
});
