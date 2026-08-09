/**
 * Quaternion helpers, w-x-y-z body-to-world, matching the convention of the
 * wire structs (body x forward, y left, z up; world z up; right-handed).
 *
 * These are the page-side twin of euler_deg() and error_angle_deg() in
 * tools/ground-station/telemetry_wire.py and must stay numerically identical
 * to them: the golden fixtures decode through both.
 */

/** Attitude quaternion, w first */
export type Quat = readonly [number, number, number, number];

/** Roll, pitch, yaw in degrees */
export type EulerDeg = [number, number, number];

const RAD_TO_DEG = 180 / Math.PI;

export function eulerDeg(q: Quat): EulerDeg {
    const [w, x, y, z] = q;
    const roll = Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y));
    const pitch = Math.asin(Math.max(-1, Math.min(1, 2 * (w * y - z * x))));
    const yaw = Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
    return [roll * RAD_TO_DEG, pitch * RAD_TO_DEG, yaw * RAD_TO_DEG];
}

/** Rotation angle between two unit quaternions, in degrees */
export function errorAngleDeg(a: Quat, b: Quat): number {
    const dot = Math.abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * Math.acos(Math.min(1, dot)) * RAD_TO_DEG;
}

/** Reads a 4-float array off a decoded message, null when it is not one */
export function asQuat(value: unknown): Quat | null {
    if (!Array.isArray(value) || value.length !== 4) {
        return null;
    }
    const q = value as number[];
    return typeof q[0] === "number" ? [q[0], q[1], q[2], q[3]] : null;
}
