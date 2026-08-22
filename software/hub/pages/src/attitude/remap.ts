/**
 * Frame remap between the wire attitude and the render frame.
 *
 * The wire quaternion is body-to-world in the drone frame (x forward, y left,
 * z up); three.js draws in y up, -z forward. RENDER_TO_DRONE is the rotation
 * mapping a render-frame vector to its drone-frame coordinates, so its
 * columns are the drone coordinates of the render x, y and z axes:
 * (0,-1,0), (0,0,1), (-1,0,0). A rotation changes frame by conjugation, so
 * the drawn attitude is m^-1 * q * m.
 *
 * No three import here: this is the piece the tests pin down, and it must
 * stay runnable without a DOM or a GL context.
 */

import type { Quat } from "../shared/quat";

/** Rotation taking render-frame coordinates to drone-frame coordinates. */
export const RENDER_TO_DRONE: Quat = [0.5, 0.5, -0.5, -0.5];

/** Hamilton product, w first. */
export function quatMul(a: Quat, b: Quat): Quat {
    const [aw, ax, ay, az] = a;
    const [bw, bx, by, bz] = b;
    return [
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    ];
}

/** A quaternion is usable when every component is finite and it is not torn. */
export function isUsable(q: Quat): boolean {
    let norm = 0;
    for (const c of q) {
        if (!Number.isFinite(c)) {
            return false;
        }
        norm += c * c;
    }
    return norm >= 0.5;
}

/** Drone-frame attitude to render-frame attitude, normalized. */
export function toRenderQuat(q: Quat): Quat {
    const m = RENDER_TO_DRONE;
    const inverse: Quat = [m[0], -m[1], -m[2], -m[3]];
    const r = quatMul(quatMul(inverse, q), m);
    const norm = Math.hypot(r[0], r[1], r[2], r[3]);
    return [r[0] / norm, r[1] / norm, r[2] / norm, r[3] / norm];
}

/** Rotates a vector by a unit quaternion; the handedness check of the remap. */
export function rotateVec(q: Quat, v: readonly [number, number, number]): [number, number, number] {
    const [w, x, y, z] = q;
    const cx = y * v[2] - z * v[1];
    const cy = z * v[0] - x * v[2];
    const cz = x * v[1] - y * v[0];
    return [
        v[0] + 2 * (w * cx + y * cz - z * cy),
        v[1] + 2 * (w * cy + z * cx - x * cz),
        v[2] + 2 * (w * cz + x * cy - y * cx),
    ];
}
