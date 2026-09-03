/**
 * Attitude panel of the control page: every live drone drawn superimposed
 * in one 3D view, one color per node - the same color its widget wears -
 * plus the translucent ghost of the exact plant attitude when a drone
 * streams one (Status.truth). Per-drone numbers live in the widgets;
 * this panel is the picture.
 */

import { isUsable, toRenderQuat } from "../attitude/remap";
import { AttitudeScene } from "../attitude/scene";
import type { GatewaySocket } from "../shared/gateway_socket";
import { nodeColor } from "../shared/nodes";
import { asQuat } from "../shared/quat";

/** Truth older than this no longer counts as a ghost to draw [ms]. */
const TRUTH_STALE_MS = 2000;

export class AttitudePanel {
    readonly root: HTMLElement;
    private readonly legend: HTMLElement;
    private readonly ghostButton: HTMLButtonElement;
    private readonly scene: AttitudeScene;
    /** Node ids drawn, with the label of each. */
    private active = new Map<number, string>();
    private ghostWanted = true;
    private truthAtMs = 0;

    constructor(socket: GatewaySocket) {
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

        // The ghost is the exact plant attitude, so it exists only while a
        // drone streams its truth: no truth, no ghost to draw or to toggle
        this.ghostButton = document.createElement("button");
        this.ghostButton.className = "btn active";
        this.ghostButton.textContent = "Ghost";
        this.ghostButton.title = "exact plant attitude, the truth behind a simulated drone";
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

        socket.onEnvelope((src, envelope) => {
            if (envelope.body.case !== "status" || !this.active.has(src)) {
                return;
            }
            const status = envelope.body.value;
            const q = asQuat(status.attitudeQuat);
            if (q !== null && isUsable(q)) {
                this.scene.setDrone(src, toRenderQuat(q), nodeColor(src));
            }
            const truth = status.truth === undefined ? null : asQuat(status.truth.attitudeQuat);
            if (truth !== null && isUsable(truth)) {
                this.scene.setExact(toRenderQuat(truth));
                const hadTruth = this.truthAtMs !== 0;
                this.truthAtMs = Date.now();
                if (!hadTruth) {
                    this.applyGhost();
                }
            }
        });

        const frame = (): void => {
            requestAnimationFrame(frame);
            if (!document.hidden) {
                // The camera moves under the pointer without any message
                // arriving, so a visible panel always redraws.
                this.scene.render();
                if (this.truthAtMs !== 0 && Date.now() - this.truthAtMs > TRUTH_STALE_MS) {
                    this.truthAtMs = 0;
                    this.applyGhost();
                }
            }
        };
        requestAnimationFrame(frame);
    }

    /** The drones currently live: gizmos and legend follow the list. */
    setActive(drones: ReadonlyMap<number, string>): void {
        this.active = new Map(drones);
        this.scene.keepDrones(new Set(this.active.keys()));
        this.applyGhost();
        this.legend.replaceChildren();
        for (const [id, label] of [...this.active].sort((a, b) => a[0] - b[0])) {
            const entry = document.createElement("span");
            entry.className = "att-legend-entry";
            const dot = document.createElement("span");
            dot.className = "lane-dot";
            dot.style.background = nodeColor(id);
            entry.appendChild(dot);
            const name = document.createElement("span");
            name.textContent = label;
            entry.appendChild(name);
            this.legend.appendChild(entry);
        }
    }

    private applyGhost(): void {
        const truthPresent = this.truthAtMs !== 0;
        this.scene.setGhostVisible(this.ghostWanted && truthPresent);
        this.ghostButton.disabled = !truthPresent;
    }
}
