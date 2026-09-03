/**
 * The layout half of the setup: the lanes in their order, one draggable chip
 * per selected series. Drop a chip on another lane to group them - only
 * among measures of the same unit, because a shared y axis is only honest
 * then - or on the trailing target to give it a lane of its own; drag a
 * lane by its grip to reorder.
 *
 * The DOM is never rebuilt while a drag is in the air: removing the source
 * element during its own dragstart cancels the drag in every browser, so
 * the feedback (which lanes can take the chip) is a class toggled on the
 * rows that exist, and the rebuild waits for the drop.
 */

import { type SeriesSpec, TelemetryModel, unitLabel } from "./model";

/** Target the layout offers for "put this series in a lane of its own". */
const NEW_LANE = -1;

export class LanesPanel {
    readonly root: HTMLElement;
    private dragging: SeriesSpec | null = null;
    private draggingLane: number | null = null;
    private readonly rows: HTMLElement;
    private readonly laneRows = new Map<number, HTMLElement>();

    /**
     * @param model the selection the panel lays out
     * @param onChanged called after every edit of the layout
     */
    constructor(
        private readonly model: TelemetryModel,
        private readonly onChanged: () => void
    ) {
        this.root = document.createElement("div");
        this.root.className = "setup-col lanes-panel";

        const bar = document.createElement("div");
        bar.className = "setup-bar";
        const title = document.createElement("span");
        title.className = "setup-title";
        title.textContent = "lanes";
        bar.appendChild(title);
        const hint = document.createElement("span");
        hint.className = "config-note";
        hint.textContent = "drag a chip onto a lane of the same unit to share its axis";
        bar.appendChild(hint);
        this.root.appendChild(bar);

        this.rows = document.createElement("div");
        this.rows.className = "lanes-rows";
        this.root.appendChild(this.rows);
    }

    /** Repaints every lane from the selection. */
    render(): void {
        this.rows.replaceChildren();
        this.laneRows.clear();
        const lanes = this.model.lanes();
        if (lanes.length === 0) {
            const empty = document.createElement("div");
            empty.className = "config-note lanes-empty";
            empty.textContent = "tick measures on the left: each one opens in a lane of its own";
            this.rows.appendChild(empty);
            return;
        }
        for (const laneId of lanes) {
            const row = this.laneRow(laneId);
            this.laneRows.set(laneId, row);
            this.rows.appendChild(row);
        }
        this.rows.appendChild(this.newLaneTarget());
    }

    private laneRow(laneId: number): HTMLElement {
        const row = document.createElement("div");
        row.className = "config-lane";
        const series = this.model.list().filter((spec) => spec.laneId === laneId);

        // The row is only draggable from its grip, so dragging a chip never
        // fights the reordering of the lane it sits in.
        const grip = document.createElement("span");
        grip.className = "config-grip";
        grip.textContent = "::";
        grip.title = "Drag to reorder the lanes";
        grip.addEventListener("mousedown", () => {
            row.draggable = true;
        });
        grip.addEventListener("mouseup", () => {
            row.draggable = false;
        });
        row.addEventListener("dragstart", (event) => {
            // A chip's dragstart bubbles up here: only the row itself counts.
            if (event.target !== row) {
                return;
            }
            this.draggingLane = laneId;
            event.dataTransfer?.setData("text/plain", `lane:${laneId}`);
        });
        row.addEventListener("dragend", () => {
            row.draggable = false;
            this.endDrag();
        });
        row.addEventListener("dragover", (event) => {
            if (this.draggingLane !== null && this.draggingLane !== laneId) {
                event.preventDefault();
                return;
            }
            if (this.dragging !== null && this.acceptsChip(laneId)) {
                event.preventDefault();
            }
        });
        row.addEventListener("drop", (event) => {
            event.preventDefault();
            if (this.draggingLane !== null && this.draggingLane !== laneId) {
                const order = this.model.lanes();
                const from = order.indexOf(this.draggingLane);
                const to = order.indexOf(laneId);
                if (from >= 0 && to >= 0) {
                    const moved = order.splice(from, 1)[0] as number;
                    order.splice(to, 0, moved);
                    this.model.orderLanes(order);
                }
            } else if (this.dragging !== null && this.acceptsChip(laneId)) {
                this.model.setLane(this.dragging.name, laneId);
            }
            this.endDrag();
            this.onChanged();
            this.render();
        });
        row.appendChild(grip);

        const title = document.createElement("span");
        title.className = "config-lane-title";
        const unit = series[0] === undefined ? "" : unitLabel(series[0].unit);
        title.textContent = unit === "" ? "unitless" : unit;
        row.appendChild(title);

        const chips = document.createElement("div");
        chips.className = "config-chips";
        for (const spec of series) {
            chips.appendChild(this.chip(spec));
        }
        row.appendChild(chips);
        return row;
    }

    /** True when a lane may take the chip currently being dragged. */
    private acceptsChip(laneId: number): boolean {
        if (this.dragging === null || this.dragging.laneId === laneId) {
            return false;
        }
        const series = this.model.list().filter((spec) => spec.laneId === laneId);
        return series.every((spec) => spec.unit === this.dragging?.unit);
    }

    /** Greys the lanes that cannot take the chip in the air. */
    private markTargets(): void {
        for (const [laneId, row] of this.laneRows) {
            row.classList.toggle("incompatible", this.dragging !== null && !this.acceptsChip(laneId));
        }
    }

    private endDrag(): void {
        this.dragging = null;
        this.draggingLane = null;
        this.markTargets();
    }

    private newLaneTarget(): HTMLElement {
        const target = document.createElement("div");
        target.className = "config-lane new-lane";
        target.textContent = "drop here for a lane of its own";
        target.addEventListener("dragover", (event) => {
            if (this.dragging !== null) {
                event.preventDefault();
            }
        });
        target.addEventListener("drop", (event) => {
            event.preventDefault();
            if (this.dragging !== null) {
                this.model.setLane(this.dragging.name, NEW_LANE);
                this.endDrag();
                this.onChanged();
                this.render();
            }
        });
        return target;
    }

    private chip(spec: SeriesSpec): HTMLElement {
        const chip = document.createElement("span");
        chip.className = "config-chip";
        chip.draggable = true;
        chip.addEventListener("dragstart", (event) => {
            event.stopPropagation();
            this.dragging = spec;
            event.dataTransfer?.setData("text/plain", spec.name);
            this.markTargets();
        });
        chip.addEventListener("dragend", () => {
            this.endDrag();
        });

        const dot = document.createElement("span");
        dot.className = "lane-dot";
        dot.style.background = spec.color;
        chip.appendChild(dot);

        const name = document.createElement("span");
        name.textContent = spec.name;
        chip.appendChild(name);

        if (this.model.isStale(spec.name)) {
            const stale = document.createElement("span");
            stale.className = "config-note";
            stale.textContent = "(absent)";
            stale.title = "this node's table does not carry that measure";
            chip.appendChild(stale);
        }

        const remove = document.createElement("span");
        remove.className = "config-chip-remove";
        remove.textContent = "x";
        remove.title = "Remove from the selection";
        remove.addEventListener("click", () => {
            this.model.remove(spec.name);
            this.onChanged();
            this.render();
        });
        chip.appendChild(remove);
        return chip;
    }
}
