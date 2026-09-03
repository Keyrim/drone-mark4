/**
 * The catalog of a node's measures as a tree, folded on the `/` of their
 * names: `estimator/attitude/pitch` is the leaf `pitch` under the folder
 * `attitude` under the folder `estimator`. DOM-free, so the tests drive it.
 *
 * A folder that would hold a single child is not worth a click: a lone
 * folder child is merged into its parent (`sim/truth` when `sim` has
 * nothing else), a lone leaf is lifted to the parent with its path as its
 * label (`rc/throttle` stays a leaf at the top level while `rc` exposes
 * nothing else).
 */

import { type Descriptor } from "./model";

/** One row of the tree: a folder (children) or a leaf (a descriptor). */
export interface TreeNode {
    /** Full path of the folder, or the name of the measure */
    readonly path: string;
    /** What the row shows: the last segment(s) not shown by the parents */
    readonly label: string;
    readonly children: TreeNode[];
    readonly leaf?: Descriptor;
}

interface Building {
    path: string;
    label: string;
    folders: Map<string, Building>;
    leaves: Descriptor[];
}

/**
 * Builds the tree of the descriptors whose name contains the filter.
 *
 * @param descriptors the node's table
 * @param filter lowercase substring a measure name must contain, "" for all
 * @returns the top-level rows, folders first, each level sorted by label
 */
export function buildTree(descriptors: readonly Descriptor[], filter = ""): TreeNode[] {
    const root: Building = { path: "", label: "", folders: new Map(), leaves: [] };
    for (const descriptor of descriptors) {
        if (filter !== "" && !descriptor.name.toLowerCase().includes(filter)) {
            continue;
        }
        const segments = descriptor.name.split("/");
        let at = root;
        for (const segment of segments.slice(0, -1)) {
            const path = at.path === "" ? segment : `${at.path}/${segment}`;
            let next = at.folders.get(path);
            if (next === undefined) {
                next = { path, label: segment, folders: new Map(), leaves: [] };
                at.folders.set(path, next);
            }
            at = next;
        }
        at.leaves.push(descriptor);
    }
    return fold(root).children;
}

/** Turns a building folder into rows, merging the folders of one child. */
function fold(folder: Building): TreeNode {
    const children: TreeNode[] = [];
    for (const sub of folder.folders.values()) {
        const built = fold(sub);
        if (built.children.length === 1 && built.children[0] !== undefined) {
            // A folder of one row: the row takes the folder's place, its
            // label prefixed so the path stays readable.
            const only = built.children[0];
            children.push({ ...only, label: `${built.label}/${only.label}` });
            continue;
        }
        children.push(built);
    }
    for (const leaf of folder.leaves) {
        children.push({
            path: leaf.name,
            label: leaf.name.slice(folder.path === "" ? 0 : folder.path.length + 1),
            children: [],
            leaf,
        });
    }
    children.sort((a, b) => {
        const folderA = a.leaf === undefined ? 0 : 1;
        const folderB = b.leaf === undefined ? 0 : 1;
        return folderA - folderB || a.label.localeCompare(b.label);
    });
    return { path: folder.path, label: folder.label, children };
}

/** Every measure under a row, the row itself when it is a leaf. */
export function leavesOf(node: TreeNode): Descriptor[] {
    if (node.leaf !== undefined) {
        return [node.leaf];
    }
    return node.children.flatMap(leavesOf);
}

/** The folder paths on the way to a measure, outermost first. */
export function ancestorsOf(name: string): string[] {
    const segments = name.split("/");
    const out: string[] = [];
    for (let i = 1; i < segments.length; i += 1) {
        out.push(segments.slice(0, i).join("/"));
    }
    return out;
}
