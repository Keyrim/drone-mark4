import assert from "node:assert/strict";
import test from "node:test";

import { asQuat, errorAngleDeg, eulerDeg, type Quat } from "../src/shared/quat";

/*
 * The two quaternions below are the ones the golden fixtures carry, read back
 * with the reference decoder. The expected angles were produced with:
 *
 *   python3 -c "import sys; sys.path.insert(0, 'tools/ground-station');
 *   import telemetry_wire as tw;
 *   t = tw.decode_telemetry(open('tests/golden/fixtures/telemetry.bin','rb').read());
 *   r = tw.decode_sim_raw(open('tests/golden/fixtures/sim_raw.bin','rb').read());
 *   print(tw.euler_deg(t.attitude_quat), tw.euler_deg(r.attitude_quat),
 *         tw.error_angle_deg(t.attitude_quat, r.attitude_quat))"
 *
 * Both fixtures store exact binary fractions, so float32 and double agree and
 * the constants can be compared tightly.
 */
const TELEMETRY_QUAT: Quat = [0.5, -0.25, 0.125, -0.0625];
const SIM_RAW_QUAT: Quat = [0.875, -0.375, 0.1875, -0.09375];
const TELEMETRY_EULER = [-17.47477425625567, 5.379378991112074, -7.411492859178873];
const SIM_RAW_EULER = [-46.836844430060964, 14.940303130903516, -18.471747648634878];
const ERROR_ANGLE = 111.81274753352453;

function assertClose(actual: number[], expected: number[]): void {
    assert.equal(actual.length, expected.length);
    for (let i = 0; i < expected.length; i++) {
        assert.ok(
            Math.abs((actual[i] as number) - (expected[i] as number)) < 1e-9,
            `index ${i}: ${actual[i]} != ${expected[i]}`
        );
    }
}

test("eulerDeg matches the reference decoder on the golden fixtures", () => {
    assertClose(eulerDeg(TELEMETRY_QUAT), TELEMETRY_EULER);
    assertClose(eulerDeg(SIM_RAW_QUAT), SIM_RAW_EULER);
});

test("eulerDeg is zero on the identity quaternion", () => {
    assertClose(eulerDeg([1, 0, 0, 0]), [0, 0, 0]);
});

test("eulerDeg clamps the pitch instead of returning NaN", () => {
    // A 90 deg pitch drives the asin argument to exactly 1; a rounding
    // overshoot past 1 must not turn the whole attitude into NaN
    const half = Math.SQRT1_2;
    const pitchUp = eulerDeg([half, 0, half, 0]);
    assert.ok(Math.abs((pitchUp[1] as number) - 90) < 1e-9);
});

test("errorAngleDeg is zero for identical attitudes and sign agnostic", () => {
    assert.equal(errorAngleDeg([1, 0, 0, 0], [1, 0, 0, 0]), 0);
    // q and -q are the same rotation
    assert.equal(errorAngleDeg([1, 0, 0, 0], [-1, 0, 0, 0]), 0);
});

test("errorAngleDeg matches the reference decoder on the golden fixtures", () => {
    assert.ok(Math.abs(errorAngleDeg(TELEMETRY_QUAT, SIM_RAW_QUAT) - ERROR_ANGLE) < 1e-9);
});

test("errorAngleDeg reads a 90 deg yaw as 90 deg", () => {
    const half = Math.SQRT1_2;
    assert.ok(Math.abs(errorAngleDeg([1, 0, 0, 0], [half, 0, 0, half]) - 90) < 1e-9);
});

test("asQuat rejects anything that is not four numbers", () => {
    assert.deepEqual(asQuat([1, 0, 0, 0]), [1, 0, 0, 0]);
    assert.equal(asQuat([1, 0, 0]), null);
    assert.equal(asQuat(undefined), null);
    assert.equal(asQuat("1,0,0,0"), null);
    assert.equal(asQuat(["a", "b", "c", "d"]), null);
});
