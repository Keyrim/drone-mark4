/**
 * Attitude panel of the control page: every connected drone drawn
 * superimposed in one 3D view, one color per drone - the same color its
 * widget wears - plus the translucent ghost of the exact simulator attitude
 * when one streams. Per-drone numbers live in the widgets; this panel is
 * the picture.
 */

import { asQuat } from "../shared/quat";
import { SOURCE_NAMES, sourceColor } from "../shared/series";
import type { HubMessage, HubSocket } from "../shared/hub_socket";
import { isUsable, toRenderQuat } from "../attitude/remap";
import { AttitudeScene } from "../attitude/scene";

/** StreamSource kind of the simulated drone, the one that owns the ghost. */
const KIND_DRONE_SIM = 2;

export class AttitudePanel {
    readonly root: HTMLElement;
    private readonly legend: HTMLElement;
    private readonly ghostButton: HTMLButtonElement;
    private readonly scene: AttitudeScene;
    private active = new Set<number>();
    private ghostWanted = true;

    constructor(socket: HubSocket) {
        this.root = document.createElement("section");
        this.root.className = "panel att-panel";

        const bar = document.createElement("div");
        bar.className = "panel-bar";
        const head = document.createElement("b");
        head.textContent = "Attitude";
        bar.appendChild(head);

        this.legend = document.createElement("span");
        this.legend.className = "att-legend";
        bar.appendChild(this.legend);

        const spacer = document.createElement("span");
        spacer.className = "bar-grow";
        bar.appendChild(spacer);

        // The ghost is the exact plant attitude, so it belongs to the
        // simulated drone: no drone_sim, no ghost to draw or to toggle
        this.ghostButton = document.createElement("button");
        this.ghostButton.className = "btn active";
        this.ghostButton.textContent = "Ghost";
        this.ghostButton.title = "exact simulator attitude, the truth behind drone_sim";
        this.ghostButton.addEventListener("click", () => {
            this.ghostWanted = !this.ghostWanted;
            this.ghostButton.classList.toggle("active", this.ghostWanted);
            this.applyGhost();
        });
        bar.appendChild(this.ghostButton);
        this.root.appendChild(bar);

        const stage = document.createElement("div");
        stage.className = "att-stage";
        this.root.appendChild(stage);

        this.scene = new AttitudeScene(stage);
        new ResizeObserver(() => this.scene.resize()).observe(stage);

        socket.on("telemetry", (message: HubMessage) => {
            const kind = Number(message["sourceId"]);
            if (!this.active.has(kind)) {
                return;
            }
            const q = asQuat(message["attitudeQuat"]);
            if (q !== null && isUsable(q)) {
                this.scene.setDrone(kind, toRenderQuat(q), sourceColor(kind));
            }
        });
        socket.on("simRaw", (message: HubMessage) => {
            const q = asQuat(message["attitudeQuat"]);
            if (q !== null && isUsable(q)) {
                this.scene.setExact(toRenderQuat(q));
            }
        });

        const frame = (): void => {
            requestAnimationFrame(frame);
            if (!document.hidden) {
                // The camera moves under the pointer without any message
                // arriving, so a visible panel always redraws.
                this.scene.render();
            }
        };
        requestAnimationFrame(frame);
    }

    /** The drones currently connected: gizmos and legend follow the list. */
    setActive(kinds: ReadonlySet<number>): void {
        this.active = new Set(kinds);
        this.scene.keepDrones(this.active);
        this.applyGhost();
        this.legend.replaceChildren();
        for (const kind of [...this.active].sort((a, b) => a - b)) {
            const entry = document.createElement("span");
            entry.className = "att-legend-entry";
            const dot = document.createElement("span");
            dot.className = "lane-dot";
            dot.style.background = sourceColor(kind);
            entry.appendChild(dot);
            const name = document.createElement("span");
            name.textContent = SOURCE_NAMES.get(kind) ?? `source ${kind}`;
            entry.appendChild(name);
            this.legend.appendChild(entry);
        }
    }

    private applyGhost(): void {
        const simPresent = this.active.has(KIND_DRONE_SIM);
        this.scene.setGhostVisible(this.ghostWanted && simPresent);
        this.ghostButton.disabled = !simPresent;
    }
}
