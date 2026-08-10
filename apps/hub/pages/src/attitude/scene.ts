/**
 * The three.js side of the attitude view: one gizmo per connected drone
 * over a grid, all superimposed at the origin, and the little orbit camera
 * that looks at them. The caller names each drone by an id and a color; the
 * translucent ghost is the exact simulator attitude, when one streams.
 *
 * Everything here draws in the render frame (y up, -z forward); the drone
 * frame never reaches this module, the caller hands over attitudes already
 * remapped by remap.ts.
 */

import * as THREE from "three";

import type { Quat } from "../shared/quat";

// The ghost wears the simulated drone's color: it is ITS truth, drawn
// translucent behind its estimate.
const EXACT_COLOR = 0x3987e5;
const GHOST_OPACITY = 0.3;

/** Camera distance bounds, in metres. */
const MIN_RADIUS = 0.35;
const MAX_RADIUS = 4;
const ORBIT_SPEED = 0.006;
const ZOOM_STEP = 1.0015;
/** Kept just short of the poles, where the up vector flips. */
const MAX_PHI = Math.PI / 2 - 0.01;

/**
 * A drone-like marker: flat frame, light nose block showing forward, dark
 * mast showing up. Enough asymmetry to read any orientation at a glance.
 */
function makeGizmo(color: number, opacity: number): THREE.Group {
    const base = new THREE.Color(color);
    const group = new THREE.Group();
    const parts: [THREE.Vector3, THREE.Vector3, THREE.Color][] = [
        [new THREE.Vector3(0.34, 0.04, 0.34), new THREE.Vector3(0, 0, 0), base],
        [
            new THREE.Vector3(0.1, 0.04, 0.14),
            new THREE.Vector3(0, 0, -0.24),
            base.clone().lerp(new THREE.Color(0xffffff), 0.4),
        ],
        [
            new THREE.Vector3(0.05, 0.16, 0.05),
            new THREE.Vector3(0, 0.1, 0),
            base.clone().multiplyScalar(0.5),
        ],
    ];
    for (const [size, position, tint] of parts) {
        const material = new THREE.MeshLambertMaterial({ color: tint });
        if (opacity < 1) {
            material.transparent = true;
            material.opacity = opacity;
            material.depthWrite = false;
        }
        const mesh = new THREE.Mesh(new THREE.BoxGeometry(size.x, size.y, size.z), material);
        mesh.position.copy(position);
        group.add(mesh);
    }
    return group;
}

/**
 * Body axes of the drone frame, drawn in render coordinates: x forward is
 * render -z, y left is render -x, z up is render +y.
 */
function makeAxes(): THREE.Group {
    const group = new THREE.Group();
    const axes: [THREE.Vector3, number][] = [
        [new THREE.Vector3(0, 0, -1), 0xe66767],
        [new THREE.Vector3(-1, 0, 0), 0x199e70],
        [new THREE.Vector3(0, 1, 0), 0x3987e5],
    ];
    for (const [direction, color] of axes) {
        group.add(new THREE.ArrowHelper(direction, new THREE.Vector3(), 0.45, color, 0.09, 0.05));
    }
    return group;
}

export class AttitudeScene {
    private readonly renderer: THREE.WebGLRenderer;
    private readonly scene = new THREE.Scene();
    private readonly camera = new THREE.PerspectiveCamera(45, 1, 0.05, 100);
    private readonly ghost = makeGizmo(EXACT_COLOR, GHOST_OPACITY);
    private readonly drones = new Map<number, THREE.Group>();
    /** Camera position in spherical coordinates around the origin. */
    private radius = 1.1;
    private theta = Math.PI / 4;
    private phi = 0.6;

    constructor(private readonly container: HTMLElement) {
        this.renderer = new THREE.WebGLRenderer({ antialias: true });
        this.renderer.setPixelRatio(devicePixelRatio);
        container.appendChild(this.renderer.domElement);

        this.scene.background = new THREE.Color(0x14171c);
        this.scene.add(new THREE.GridHelper(3, 12, 0x3d4756, 0x272d36));
        this.scene.add(new THREE.AmbientLight(0xffffff, 1.4));
        const sun = new THREE.DirectionalLight(0xffffff, 2.0);
        sun.position.set(1, 2, 1.5);
        this.scene.add(sun);

        this.scene.add(this.ghost);

        this.bindOrbit(this.renderer.domElement);
        this.resize();
    }

    /** Attitude of one drone's gizmo, created on first sight in its color. */
    setDrone(id: number, q: Quat, color: string): void {
        let gizmo = this.drones.get(id);
        if (gizmo === undefined) {
            gizmo = makeGizmo(new THREE.Color(color).getHex(), 1);
            gizmo.add(makeAxes());
            this.drones.set(id, gizmo);
            this.scene.add(gizmo);
        }
        gizmo.quaternion.set(q[1], q[2], q[3], q[0]);
    }

    /** Drop the gizmos of drones that are gone. */
    keepDrones(ids: ReadonlySet<number>): void {
        for (const [id, gizmo] of this.drones) {
            if (!ids.has(id)) {
                this.scene.remove(gizmo);
                this.drones.delete(id);
            }
        }
    }

    setExact(q: Quat): void {
        this.ghost.quaternion.set(q[1], q[2], q[3], q[0]);
    }

    setGhostVisible(visible: boolean): void {
        this.ghost.visible = visible;
    }

    resize(): void {
        const width = Math.max(1, this.container.clientWidth);
        const height = Math.max(1, this.container.clientHeight);
        this.renderer.setSize(width, height);
        this.camera.aspect = width / height;
        this.camera.updateProjectionMatrix();
    }

    render(): void {
        this.camera.position.set(
            this.radius * Math.cos(this.phi) * Math.sin(this.theta),
            this.radius * Math.sin(this.phi),
            this.radius * Math.cos(this.phi) * Math.cos(this.theta)
        );
        this.camera.lookAt(0, 0, 0);
        this.renderer.render(this.scene, this.camera);
    }

    /** Drag to orbit, wheel to zoom. Pointer capture handles leaving the canvas. */
    private bindOrbit(canvas: HTMLCanvasElement): void {
        let dragging = -1;
        let lastX = 0;
        let lastY = 0;
        canvas.addEventListener("pointerdown", (event: PointerEvent) => {
            dragging = event.pointerId;
            lastX = event.clientX;
            lastY = event.clientY;
            canvas.setPointerCapture(event.pointerId);
        });
        canvas.addEventListener("pointermove", (event: PointerEvent) => {
            if (event.pointerId !== dragging) {
                return;
            }
            this.theta -= (event.clientX - lastX) * ORBIT_SPEED;
            this.phi = Math.max(
                -MAX_PHI,
                Math.min(MAX_PHI, this.phi + (event.clientY - lastY) * ORBIT_SPEED)
            );
            lastX = event.clientX;
            lastY = event.clientY;
        });
        const release = (event: PointerEvent): void => {
            if (event.pointerId === dragging) {
                dragging = -1;
                canvas.releasePointerCapture(event.pointerId);
            }
        };
        canvas.addEventListener("pointerup", release);
        canvas.addEventListener("pointercancel", release);
        canvas.addEventListener(
            "wheel",
            (event: WheelEvent) => {
                event.preventDefault();
                const scaled = this.radius * Math.pow(ZOOM_STEP, event.deltaY);
                this.radius = Math.max(MIN_RADIUS, Math.min(MAX_RADIUS, scaled));
            },
            { passive: false }
        );
    }
}
