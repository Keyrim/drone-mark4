/**
 * Lane configuration panel: which series each lane draws, saved under a name
 * in the browser. These are view configs, nothing is recorded here.
 */

import { LIVE_SERIES, type SeriesDef } from "../shared/series";
import {
    loadViewConfigs,
    moveLane,
    saveViewConfigs,
    type LaneConfig,
    type ViewConfig,
} from "./model";

/** Name shown for the built-in lanes, which are never overwritten. */
export const DEFAULT_NAME = "default";

export class ConfigPanel {
    readonly root: HTMLElement;
    private lanes: LaneConfig[];
    private configs: ViewConfig[] = loadViewConfigs();
    private current = DEFAULT_NAME;
    private dragFrom: number | null = null;
    private readonly picker: HTMLSelectElement;
    private readonly laneList: HTMLElement;

    constructor(
        private readonly defaults: LaneConfig[],
        private readonly onChanged: (lanes: LaneConfig[]) => void
    ) {
        this.lanes = clone(defaults);

        this.root = document.createElement("div");
        this.root.className = "config-panel";

        const bar = document.createElement("div");
        bar.className = "config-bar";

        this.picker = document.createElement("select");
        this.picker.className = "config-select";
        this.picker.addEventListener("change", () => this.select(this.picker.value));
        bar.appendChild(this.picker);

        bar.appendChild(this.button("Save as...", () => this.saveAs()));
        bar.appendChild(this.button("Delete", () => this.remove()));
        bar.appendChild(this.button("Add lane", () => this.addLane()));
        this.root.appendChild(bar);

        this.laneList = document.createElement("div");
        this.laneList.className = "config-lanes";
        this.root.appendChild(this.laneList);

        this.refreshPicker();
        this.render();
    }

    /** Lanes currently configured. */
    currentLanes(): LaneConfig[] {
        return this.lanes;
    }

    private button(text: string, onClick: () => void): HTMLButtonElement {
        const button = document.createElement("button");
        button.className = "btn";
        button.textContent = text;
        button.addEventListener("click", onClick);
        return button;
    }

    private refreshPicker(): void {
        this.picker.replaceChildren();
        for (const name of [DEFAULT_NAME, ...this.configs.map((c) => c.name)]) {
            const option = document.createElement("option");
            option.value = name;
            option.textContent = name;
            this.picker.appendChild(option);
        }
        this.picker.value = this.current;
    }

    private select(name: string): void {
        const found = this.configs.find((c) => c.name === name);
        this.current = found ? name : DEFAULT_NAME;
        this.lanes = clone(found ? found.lanes : this.defaults);
        this.render();
        this.onChanged(this.lanes);
    }

    private saveAs(): void {
        const name = prompt("View name", this.current === DEFAULT_NAME ? "" : this.current);
        if (name === null || name.trim() === "" || name.trim() === DEFAULT_NAME) {
            return;
        }
        this.current = name.trim();
        this.configs = [
            ...this.configs.filter((c) => c.name !== this.current),
            { name: this.current, lanes: clone(this.lanes) },
        ];
        saveViewConfigs(this.configs);
        this.refreshPicker();
    }

    private remove(): void {
        if (this.current === DEFAULT_NAME) {
            return;
        }
        this.configs = this.configs.filter((c) => c.name !== this.current);
        saveViewConfigs(this.configs);
        this.select(DEFAULT_NAME);
        this.refreshPicker();
    }

    private addLane(): void {
        this.lanes.push({ title: `lane ${this.lanes.length + 1}`, keys: [] });
        this.changed();
    }

    /** Persist the working lanes under the current name, then repaint. */
    private changed(): void {
        if (this.current !== DEFAULT_NAME) {
            const entry = this.configs.find((c) => c.name === this.current);
            if (entry) {
                entry.lanes = clone(this.lanes);
                saveViewConfigs(this.configs);
            }
        }
        this.render();
        this.onChanged(this.lanes);
    }

    private render(): void {
        this.laneList.replaceChildren();
        this.lanes.forEach((lane, index) => this.laneList.appendChild(this.laneRow(lane, index)));
    }

    private laneRow(lane: LaneConfig, index: number): HTMLElement {
        const row = document.createElement("div");
        row.className = "config-lane";

        // The row is only draggable from its grip, so dragging never fights
        // the text inputs and selects living on the same line.
        const grip = document.createElement("span");
        grip.className = "config-grip";
        grip.textContent = "::";
        grip.title = "Drag to reorder";
        grip.addEventListener("mousedown", () => {
            row.draggable = true;
        });
        row.addEventListener("dragstart", (event) => {
            this.dragFrom = index;
            event.dataTransfer?.setData("text/plain", String(index));
        });
        row.addEventListener("dragend", () => {
            row.draggable = false;
            this.dragFrom = null;
        });
        row.addEventListener("dragover", (event) => {
            if (this.dragFrom !== null && this.dragFrom !== index) {
                event.preventDefault();
            }
        });
        row.addEventListener("drop", (event) => {
            event.preventDefault();
            if (this.dragFrom !== null && this.dragFrom !== index) {
                moveLane(this.lanes, this.dragFrom, index);
                this.changed();
            }
            this.dragFrom = null;
        });
        row.appendChild(grip);

        const title = document.createElement("input");
        title.className = "config-title";
        title.value = lane.title;
        title.addEventListener("change", () => {
            lane.title = title.value;
            this.changed();
        });
        row.appendChild(title);

        const chips = document.createElement("div");
        chips.className = "config-chips";
        for (const key of lane.keys) {
            const def = LIVE_SERIES.find((d) => d.key === key);
            if (def) {
                chips.appendChild(this.chip(def, lane));
            }
        }
        row.appendChild(chips);

        const add = document.createElement("select");
        add.className = "config-select";
        const head = document.createElement("option");
        head.value = "";
        head.textContent = "add series...";
        add.appendChild(head);
        for (const def of LIVE_SERIES) {
            if (lane.keys.includes(def.key)) {
                continue;
            }
            const option = document.createElement("option");
            option.value = def.key;
            option.textContent = def.unit === "" ? def.label : `${def.label} [${def.unit}]`;
            add.appendChild(option);
        }
        add.addEventListener("change", () => {
            if (add.value !== "") {
                lane.keys.push(add.value);
                this.changed();
            }
        });
        row.appendChild(add);

        const drop = document.createElement("button");
        drop.className = "btn danger";
        drop.textContent = "x";
        drop.title = "Remove this lane";
        drop.addEventListener("click", () => {
            this.lanes.splice(index, 1);
            this.changed();
        });
        row.appendChild(drop);
        return row;
    }

    private chip(def: SeriesDef, lane: LaneConfig): HTMLElement {
        const chip = document.createElement("span");
        chip.className = "config-chip";
        const dot = document.createElement("span");
        dot.className = "lane-dot";
        dot.style.background = def.color;
        const name = document.createElement("span");
        name.textContent = def.label;
        const remove = document.createElement("span");
        remove.className = "config-chip-remove";
        remove.textContent = "x";
        remove.addEventListener("click", () => {
            lane.keys = lane.keys.filter((k) => k !== def.key);
            this.changed();
        });
        chip.appendChild(dot);
        chip.appendChild(name);
        chip.appendChild(remove);
        return chip;
    }
}

function clone(lanes: LaneConfig[]): LaneConfig[] {
    return lanes.map((lane) => ({ title: lane.title, keys: [...lane.keys] }));
}
