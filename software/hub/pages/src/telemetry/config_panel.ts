/**
 * The setup of the telemetry page: which measures of the source node are
 * recorded, at what period, and how they are laid out in lanes. Two columns
 * filling the page, the catalog tree on the left (`catalog.ts`) and the
 * lanes on the right (`lanes_panel.ts`); the period control is handed to
 * the toolbar, next to the config it belongs to.
 *
 * The catalog is the node's own descriptor table, pulled by the gateway;
 * there is no hardcoded list of series anywhere.
 */

import { Catalog } from "./catalog";
import { LanesPanel } from "./lanes_panel";
import { type Descriptor, TelemetryModel } from "./model";

/** Bounds of the period a subscriber may ask for [ms]. */
export const MIN_PERIOD_MS = 1;
export const MAX_PERIOD_MS = 5000;
export const DEFAULT_PERIOD_MS = 50;

export class ConfigPanel {
    /** The two columns, shown in setup mode only. */
    readonly root: HTMLElement;
    /** The period field and its note, for the toolbar. */
    readonly periodControl: HTMLElement;
    private periodMs = DEFAULT_PERIOD_MS;
    private effectivePeriodMs: number | null = null;
    private readonly catalog: Catalog;
    private readonly lanes: LanesPanel;
    private readonly periodInput: HTMLInputElement;
    private readonly periodNote: HTMLElement;

    /**
     * @param model selection the panel edits
     * @param onChanged called after every edit of the selection or the period
     */
    constructor(model: TelemetryModel, onChanged: () => void) {
        this.root = document.createElement("div");
        this.root.className = "setup";

        // Either column edits the same selection, so an edit on one side
        // repaints the other.
        this.catalog = new Catalog(model, () => {
            this.lanes.render();
            onChanged();
        });
        this.lanes = new LanesPanel(model, () => {
            this.catalog.render();
            onChanged();
        });
        this.root.appendChild(this.catalog.root);
        this.root.appendChild(this.lanes.root);

        this.periodControl = document.createElement("label");
        this.periodControl.className = "config-period";
        this.periodControl.textContent = "period";
        this.periodInput = document.createElement("input");
        this.periodInput.type = "number";
        this.periodInput.min = String(MIN_PERIOD_MS);
        this.periodInput.max = String(MAX_PERIOD_MS);
        this.periodInput.value = String(this.periodMs);
        this.periodInput.addEventListener("change", () => {
            this.periodMs = clampPeriod(Number(this.periodInput.value));
            this.periodInput.value = String(this.periodMs);
            onChanged();
            this.renderPeriod();
        });
        this.periodControl.appendChild(this.periodInput);
        const ms = document.createElement("span");
        ms.textContent = "ms";
        this.periodControl.appendChild(ms);
        this.periodNote = document.createElement("span");
        this.periodNote.className = "config-note";
        this.periodControl.appendChild(this.periodNote);

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

    /** The node's catalog changed: rebuild the tree and the lanes. */
    setDescriptors(descriptors: readonly Descriptor[]): void {
        this.catalog.setDescriptors(descriptors);
        this.lanes.render();
    }

    /** Opens the folders holding a selected measure: after a config load. */
    revealSelection(): void {
        this.catalog.revealSelection();
    }

    /** Repaints everything: after a config load or a rebind. */
    render(): void {
        this.renderPeriod();
        this.catalog.render();
        this.lanes.render();
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
}

/** The period a subscriber may actually ask for [ms]. */
export function clampPeriod(requested: number): number {
    if (!Number.isFinite(requested)) {
        return DEFAULT_PERIOD_MS;
    }
    return Math.min(MAX_PERIOD_MS, Math.max(MIN_PERIOD_MS, Math.round(requested)));
}
