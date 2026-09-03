/**
 * The catalog of the source node's measures, as a tree: folders on the `/`
 * of the names, a tri-state box on every folder, a box, a colour dot and a
 * unit on every leaf. Ticking a folder selects everything under it; the
 * folders left open are remembered per browser.
 *
 * The rows are the node's own descriptor table (`tree.ts`), never a list of
 * this page's: a measure added to the firmware shows up here by itself.
 */

import { type Descriptor, TelemetryModel, unitLabel } from "./model";
import { loadLocal, saveLocal, TREE_STATE_KEY } from "./store";
import { ancestorsOf, buildTree, leavesOf, type TreeNode } from "./tree";

export class Catalog {
    readonly root: HTMLElement;
    private descriptors: readonly Descriptor[] = [];
    private filter = "";
    private readonly expanded = new Set<string>();
    private readonly filterInput: HTMLInputElement;
    private readonly note: HTMLElement;
    private readonly rows: HTMLElement;

    /**
     * @param model the selection the catalog edits
     * @param onChanged called after every edit of the selection
     */
    constructor(
        private readonly model: TelemetryModel,
        private readonly onChanged: () => void
    ) {
        this.root = document.createElement("div");
        this.root.className = "setup-col catalog";

        const bar = document.createElement("div");
        bar.className = "setup-bar";
        this.filterInput = document.createElement("input");
        this.filterInput.className = "config-title";
        this.filterInput.placeholder = "filter measures";
        this.filterInput.addEventListener("input", () => {
            this.filter = this.filterInput.value.trim().toLowerCase();
            this.render();
        });
        bar.appendChild(this.filterInput);
        this.note = document.createElement("span");
        this.note.className = "config-note";
        bar.appendChild(this.note);
        this.root.appendChild(bar);

        this.rows = document.createElement("div");
        this.rows.className = "tree";
        this.root.appendChild(this.rows);

        for (const path of storedPaths()) {
            this.expanded.add(path);
        }
    }

    /** The node's table changed: rebuild the rows. */
    setDescriptors(descriptors: readonly Descriptor[]): void {
        this.descriptors = descriptors;
        this.render();
    }

    /** Opens every folder on the way to a selected measure. */
    revealSelection(): void {
        for (const spec of this.model.list()) {
            for (const path of ancestorsOf(spec.name)) {
                this.expanded.add(path);
            }
        }
        this.remember();
        this.render();
    }

    /** Repaints the tree from the table, the filter and the selection. */
    render(): void {
        this.rows.replaceChildren();
        const selected = this.model.list().length;
        this.note.textContent =
            this.descriptors.length === 0
                ? "the gateway is still asking this node for its measures"
                : `${selected} of ${this.descriptors.length} selected`;
        for (const node of buildTree(this.descriptors, this.filter)) {
            this.renderNode(node, 0);
        }
    }

    private renderNode(node: TreeNode, depth: number): void {
        const row = document.createElement("div");
        row.className = node.leaf === undefined ? "tree-row folder" : "tree-row";
        row.style.paddingLeft = `${8 + depth * 16}px`;

        const leaves = leavesOf(node);
        const ticked = leaves.filter((leaf) => this.model.has(leaf.name)).length;
        // A filter shows what it matched: every folder on the way is open.
        const open = this.filter !== "" || this.expanded.has(node.path);

        const twisty = document.createElement("span");
        twisty.className = "tree-twisty";
        if (node.leaf === undefined) {
            twisty.classList.add(open ? "open" : "closed");
            twisty.addEventListener("click", () => {
                if (this.expanded.has(node.path)) {
                    this.expanded.delete(node.path);
                } else {
                    this.expanded.add(node.path);
                }
                this.remember();
                this.render();
            });
        }
        row.appendChild(twisty);

        const tick = document.createElement("input");
        tick.type = "checkbox";
        tick.checked = leaves.length > 0 && ticked === leaves.length;
        tick.indeterminate = ticked > 0 && ticked < leaves.length;
        tick.addEventListener("change", () => {
            if (node.leaf !== undefined) {
                if (tick.checked) {
                    this.model.add(node.leaf);
                } else {
                    this.model.remove(node.leaf.name);
                }
            } else if (tick.checked) {
                this.model.addGroup(leaves);
            } else {
                for (const leaf of leaves) {
                    this.model.remove(leaf.name);
                }
            }
            this.onChanged();
            this.render();
        });
        row.appendChild(tick);

        if (node.leaf !== undefined) {
            const dot = document.createElement("span");
            dot.className = "lane-dot";
            const spec = this.model.list().find((series) => series.name === node.leaf?.name);
            dot.style.background = spec?.color ?? "transparent";
            dot.style.visibility = spec === undefined ? "hidden" : "visible";
            row.appendChild(dot);
        }

        const label = document.createElement("span");
        label.className = "tree-label";
        label.textContent = node.label;
        label.title = node.path;
        label.addEventListener("click", () => {
            if (node.leaf === undefined) {
                twisty.dispatchEvent(new Event("click"));
            } else {
                tick.click();
            }
        });
        row.appendChild(label);

        const trailing = document.createElement("span");
        trailing.className = "config-note";
        if (node.leaf !== undefined) {
            trailing.textContent = unitLabel(node.leaf.unit);
        } else {
            // A folder reads in one unit when every measure under it does:
            // the one case a folder tick lands everything in a single lane.
            const units = new Set(leaves.map((leaf) => leaf.unit));
            const unit = units.size === 1 && leaves[0] !== undefined ? unitLabel(leaves[0].unit) : "";
            trailing.textContent = unit === "" ? `${ticked}/${leaves.length}` : `${ticked}/${leaves.length}  ${unit}`;
        }
        row.appendChild(trailing);

        this.rows.appendChild(row);
        if (node.leaf === undefined && open) {
            for (const child of node.children) {
                this.renderNode(child, depth + 1);
            }
        }
    }

    private remember(): void {
        saveLocal(TREE_STATE_KEY, JSON.stringify([...this.expanded]));
    }
}

/** The folder paths left open last time, none when nothing usable is stored. */
function storedPaths(): string[] {
    try {
        const parsed: unknown = JSON.parse(loadLocal(TREE_STATE_KEY) ?? "[]");
        return Array.isArray(parsed) ? parsed.filter((path): path is string => typeof path === "string") : [];
    } catch {
        return [];
    }
}
