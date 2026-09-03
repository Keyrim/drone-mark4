/**
 * The configuring phase of the telemetry page: which measures of the source
 * node are recorded, at what period, and how they are laid out in lanes.
 *
 * The catalog is the node's own descriptor table, pulled by the gateway;
 * there is no hardcoded list of series anywhere. A new selection opens in a
 * lane of its own, and lanes are grouped by dropping one chip onto another
 * lane - only among measures that read in the same unit, because a shared
 * y axis is only honest then.
 */

import { type Descriptor, type SeriesSpec, TelemetryModel, unitLabel } from "./model";

/** Bounds of the period a subscriber may ask for [ms]. */
export const MIN_PERIOD_MS = 1;
export const MAX_PERIOD_MS = 5000;
export const DEFAULT_PERIOD_MS = 50;

/** Target the layout offers for "put this series in a lane of its own". */
const NEW_LANE = -1;

export class ConfigPanel {
    readonly root: HTMLElement;
    private descriptors: readonly Descriptor[] = [];
    private periodMs = DEFAULT_PERIOD_MS;
    private effectivePeriodMs: number | null = null;
    private filter = "";
    private dragging: SeriesSpec | null = null;
    private draggingLane: number | null = null;
    private readonly catalog: HTMLElement;
    private readonly layout: HTMLElement;
    private readonly periodInput: HTMLInputElement;
    private readonly periodNote: HTMLElement;
    private readonly filterInput: HTMLInputElement;
    private readonly emptyNote: HTMLElement;

    /**
     * @param model selection the panel edits
     * @param onChanged called after every edit of the selection or the period
     */
    constructor(
        private readonly model: TelemetryModel,
        private readonly onChanged: () => void
    ) {
        this.root = document.createElement("div");
        this.root.className = "config-panel";

        const bar = document.createElement("div");
        bar.className = "config-bar";

        this.filterInput = document.createElement("input");
        this.filterInput.className = "config-title";
        this.filterInput.placeholder = "filter measures";
        this.filterInput.addEventListener("input", () => {
            this.filter = this.filterInput.value.trim().toLowerCase();
            this.renderCatalog();
        });
        bar.appendChild(this.filterInput);

        const periodLabel = document.createElement("label");
        periodLabel.className = "config-period";
        periodLabel.textContent = "period";
        this.periodInput = document.createElement("input");
        this.periodInput.type = "number";
        this.periodInput.min = String(MIN_PERIOD_MS);
        this.periodInput.max = String(MAX_PERIOD_MS);
        this.periodInput.value = String(this.periodMs);
        this.periodInput.addEventListener("change", () => {
            this.periodMs = clampPeriod(Number(this.periodInput.value));
            this.periodInput.value = String(this.periodMs);
            this.onChanged();
            this.renderPeriod();
        });
        periodLabel.appendChild(this.periodInput);
        const ms = document.createElement("span");
        ms.textContent = "ms";
        periodLabel.appendChild(ms);
        bar.appendChild(periodLabel);

        this.periodNote = document.createElement("span");
        this.periodNote.className = "config-note";
        bar.appendChild(this.periodNote);
        this.root.appendChild(bar);

        this.emptyNote = document.createElement("div");
        this.emptyNote.className = "config-note";
        this.root.appendChild(this.emptyNote);

        this.catalog = document.createElement("div");
        this.catalog.className = "config-catalog";
        this.root.appendChild(this.catalog);

        this.layout = document.createElement("div");
        this.layout.className = "config-lanes";
        this.root.appendChild(this.layout);

        this.render();
    }

    /** Period the page asks the node for [ms]. */
    period(): number {
        return this.periodMs;
    }

    /** Sets the period, clamped; used when a config is loaded. */
    setPeriod(periodMs: number): void {
        this.periodMs = clampPeriod(periodMs);
        this.periodInput.value = String(this.periodMs);
        this.renderPeriod();
    }

    /** The period the node acknowledged, shown next to the asked one. */
    setEffectivePeriod(periodMs: number | null): void {
        this.effectivePeriodMs = periodMs;
        this.renderPeriod();
    }

    /** Freezes or unfreezes every control: the config is fixed while recording. */
    setFrozen(frozen: boolean): void {
        this.root.classList.toggle("frozen", frozen);
        this.periodInput.disabled = frozen;
        this.filterInput.disabled = frozen;
        for (const input of this.catalog.querySelectorAll("input")) {
            (input as HTMLInputElement).disabled = frozen;
        }
    }

    /** The node's catalog changed: rebuild the rows and the layout. */
    setDescriptors(descriptors: readonly Descriptor[]): void {
        this.descriptors = descriptors;
        this.render();
    }

    /** Repaints everything: after a config load, a session open, a rebind. */
    render(): void {
        this.renderPeriod();
        this.renderCatalog();
        this.renderLayout();
    }

    private renderPeriod(): void {
        if (this.effectivePeriodMs === null) {
            this.periodNote.textContent = "";
            return;
        }
        this.periodNote.textContent =
            this.effectivePeriodMs === this.periodMs
                ? `node: ${this.effectivePeriodMs} ms`
                : `node clamped to ${this.effectivePeriodMs} ms`;
    }

    private renderCatalog(): void {
        this.catalog.replaceChildren();
        this.emptyNote.textContent =
            this.descriptors.length === 0
                ? "the gateway is still asking this node for its measures"
                : "";
        for (const descriptor of this.descriptors) {
            if (this.filter !== "" && !descriptor.name.toLowerCase().includes(this.filter)) {
                continue;
            }
            this.catalog.appendChild(this.catalogRow(descriptor));
        }
    }

    private catalogRow(descriptor: Descriptor): HTMLElement {
        const row = document.createElement("label");
        row.className = "config-measure";

        const tick = document.createElement("input");
        tick.type = "checkbox";
        tick.checked = this.model.has(descriptor.name);
        tick.addEventListener("change", () => {
            if (tick.checked) {
                this.model.add(descriptor);
            } else {
                this.model.remove(descriptor.name);
            }
            this.onChanged();
            this.render();
        });
        row.appendChild(tick);

        const dot = document.createElement("span");
        dot.className = "lane-dot";
        const spec = this.model.list().find((series) => series.name === descriptor.name);
        dot.style.background = spec?.color ?? "transparent";
        dot.style.visibility = spec === undefined ? "hidden" : "visible";
        row.appendChild(dot);

        const name = document.createElement("span");
        name.className = "config-measure-name";
        name.textContent = descriptor.name;
        row.appendChild(name);

        const unit = document.createElement("span");
        unit.className = "config-note";
        unit.textContent = unitLabel(descriptor.unit);
        row.appendChild(unit);
        return row;
    }

    private renderLayout(): void {
        this.layout.replaceChildren();
        const lanes = this.model.lanes();
        for (const laneId of lanes) {
            this.layout.appendChild(this.laneRow(laneId));
        }
        if (this.model.list().length > 0) {
            this.layout.appendChild(this.newLaneTarget());
        }
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
        row.addEventListener("dragstart", (event) => {
            this.draggingLane = laneId;
            event.dataTransfer?.setData("text/plain", `lane:${laneId}`);
        });
        row.addEventListener("dragend", () => {
            row.draggable = false;
            this.dragging = null;
            this.draggingLane = null;
            this.renderLayout();
        });
        row.addEventListener("dragover", (event) => {
            if (this.draggingLane !== null && this.draggingLane !== laneId) {
                event.preventDefault();
                return;
            }
            // A chip may only join a lane whose series read in the same
            // unit: a lane with two units on one y axis says nothing.
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
            this.dragging = null;
            this.draggingLane = null;
            this.onChanged();
            this.render();
        });
        row.appendChild(grip);

        const title = document.createElement("span");
        title.className = "config-title";
        const unit = series[0] === undefined ? "" : unitLabel(series[0].unit);
        title.textContent = unit === "" ? "unitless" : unit;
        row.appendChild(title);

        // Greyed while a chip is in the air and this lane cannot take it.
        if (this.dragging !== null && !this.acceptsChip(laneId)) {
            row.classList.add("incompatible");
        }

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
                this.dragging = null;
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
            this.dragging = spec;
            event.dataTransfer?.setData("text/plain", spec.name);
            this.renderLayout();
        });
        chip.addEventListener("dragend", () => {
            this.dragging = null;
            this.renderLayout();
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
        remove.addEventListener("click", () => {
            this.model.remove(spec.name);
            this.onChanged();
            this.render();
        });
        chip.appendChild(remove);
        return chip;
    }
}

/** The period a subscriber may actually ask for [ms]. */
export function clampPeriod(requested: number): number {
    if (!Number.isFinite(requested)) {
        return DEFAULT_PERIOD_MS;
    }
    return Math.min(MAX_PERIOD_MS, Math.max(MIN_PERIOD_MS, Math.round(requested)));
}
