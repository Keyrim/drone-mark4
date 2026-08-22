/**
 * Types for the slice of three.js this page uses. The three package ships no
 * declarations of its own and @types/three is not in the dependency set, so
 * the subset lives here, next to its only consumer.
 *
 * Delete this file the day @types/three is installed.
 */

declare module "three" {
    export class Vector3 {
        constructor(x?: number, y?: number, z?: number);
        x: number;
        y: number;
        z: number;
        set(x: number, y: number, z: number): this;
        copy(other: Vector3): this;
    }

    export class Quaternion {
        set(x: number, y: number, z: number, w: number): this;
    }

    export class Color {
        constructor(color?: number | string);
        clone(): Color;
        lerp(other: Color, alpha: number): this;
        multiplyScalar(scalar: number): this;
        getHex(): number;
    }

    export class Object3D {
        readonly position: Vector3;
        readonly quaternion: Quaternion;
        visible: boolean;
        add(...objects: Object3D[]): this;
        remove(...objects: Object3D[]): this;
    }

    export class Group extends Object3D {}

    export class Scene extends Object3D {
        background: Color | null;
    }

    export class Camera extends Object3D {
        lookAt(x: number, y: number, z: number): void;
    }

    export class PerspectiveCamera extends Camera {
        constructor(fov?: number, aspect?: number, near?: number, far?: number);
        aspect: number;
        updateProjectionMatrix(): void;
    }

    export class BufferGeometry {}

    export class BoxGeometry extends BufferGeometry {
        constructor(width?: number, height?: number, depth?: number);
    }

    export class Material {
        transparent: boolean;
        opacity: number;
        depthWrite: boolean;
    }

    export class MeshLambertMaterial extends Material {
        constructor(parameters?: { color?: Color | number });
    }

    export class Mesh extends Object3D {
        constructor(geometry: BufferGeometry, material: Material);
    }

    export class Light extends Object3D {}

    export class AmbientLight extends Light {
        constructor(color?: number, intensity?: number);
    }

    export class DirectionalLight extends Light {
        constructor(color?: number, intensity?: number);
    }

    export class GridHelper extends Object3D {
        constructor(size?: number, divisions?: number, color1?: number, color2?: number);
    }

    export class ArrowHelper extends Object3D {
        constructor(
            dir?: Vector3,
            origin?: Vector3,
            length?: number,
            color?: number,
            headLength?: number,
            headWidth?: number
        );
    }

    export class WebGLRenderer {
        constructor(parameters?: { antialias?: boolean });
        readonly domElement: HTMLCanvasElement;
        setPixelRatio(value: number): void;
        setSize(width: number, height: number, updateStyle?: boolean): void;
        render(scene: Scene, camera: Camera): void;
    }
}
