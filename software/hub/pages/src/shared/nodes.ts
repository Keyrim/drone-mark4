/**
 * The node model of the pages: the system is a set of transport nodes,
 * known by node id, fed by the gateway's NodeTable and refreshed by the
 * frames that arrive between two tables. Every UI element is keyed by node
 * id; the kind of a node (from its Announce) only decides what it gets (a
 * drone gets a widget, anything else is only routing).
 *
 * Pure: no DOM, no socket. The console and the plots build on it, and the
 * tests drive it directly.
 */

import { type Node, type NodeTable } from "../gen/gateway_pb";
import { type Announce, type LogModuleInfo, NodeKind } from "../gen/mark4_pb";

/** How a page reads one node. */
export interface NodeView {
    readonly id: number;
    readonly kind: NodeKind;
    /** Kind as a word, "unknown" until the node has announced. */
    readonly kindName: string;
    /** Announce name, or the kind name when the node has none. */
    readonly name: string;
    readonly address: string;
    readonly ageMs: number;
    readonly received: number;
    readonly lost: number;
    /** The node was built on another mark4.proto than the gateway: listed, and mute. */
    readonly wireMismatch: boolean;
    readonly announce: Announce | undefined;
    /** The node's log modules and levels, as last published; empty until then. */
    readonly logModules: readonly LogModuleInfo[];
}

/** A node id as every log line prints it: 8 hex digits. */
export function hexNodeId(id: number): string {
    return (id >>> 0).toString(16).padStart(8, "0");
}

/**
 * How a node is named on screen, wherever one line has to carry both: its
 * name, then its id as 8 hex digits. Selectors use it whole; the widget
 * headers split it in two, a title and a muted id.
 */
export function nodeLabel(node: { id: number; name: string }): string {
    return `${node.name} ${hexNodeId(node.id)}`;
}

/**
 * A node heard this long ago is fading: the widget dims, and the node
 * leaves for good when the gateway drops it from the table.
 */
export const FADING_MS = 1500;

/** The name of a node's log module, or "#id" when its table does not list it. */
export function logModuleName(node: NodeView | undefined, moduleId: number): string {
    return node?.logModules.find((module) => module.id === moduleId)?.name ?? `#${moduleId}`;
}

export const KIND_NAMES: Record<number, string> = {
    [NodeKind.NODE_KIND_UNSPECIFIED]: "unknown",
    [NodeKind.FIRMWARE]: "firmware",
    [NodeKind.DRONE_SIM]: "drone_sim",
    [NodeKind.PLANT]: "plant",
    [NodeKind.GATEWAY]: "gateway",
    [NodeKind.BATCH]: "batch",
};

export function kindName(kind: NodeKind): string {
    return KIND_NAMES[kind] ?? `kind ${kind}`;
}

/** A drone is what gets a widget: the board, or a desktop flight process. */
export function isDrone(node: { kind: NodeKind }): boolean {
    return node.kind === NodeKind.FIRMWARE || node.kind === NodeKind.DRONE_SIM;
}

/**
 * Categorical palette (validated: colour-vision-deficiency separation,
 * chroma, lightness band and contrast against a dark surface).
 */
export const PALETTE = [
    "#3987e5",
    "#199e70",
    "#c98500",
    "#008300",
    "#9085e9",
    "#e66767",
    "#d55181",
    "#d95926",
];

/**
 * One stable color per node id, shared by the widgets and the 3D view so
 * an operator maps them at a glance. A hash of the id, so the
 * same drone wears the same color in every tab and after a reload.
 */
export function nodeColor(id: number): string {
    // ponytail: 8 colors, two nodes may collide; a per-table assignment
    // would avoid it at the cost of colors changing as nodes come and go.
    let hash = id >>> 0;
    hash ^= hash >>> 16;
    hash = Math.imul(hash, 0x45d9f3b) >>> 0;
    hash ^= hash >>> 16;
    return PALETTE[hash % PALETTE.length] as string;
}

/** True when the node's schema hash differs from the gateway's (0 = unknown, never flagged). */
export function wireMismatch(announce: Announce | undefined, gatewayWireHash: number): boolean {
    return announce !== undefined && gatewayWireHash !== 0 && announce.wireHash !== gatewayWireHash;
}

function toView(node: Node, gatewayWireHash: number): NodeView {
    const kind = node.announce?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED;
    const word = kindName(kind);
    return {
        id: node.id,
        kind,
        kindName: word,
        name: node.announce?.name ? node.announce.name : word,
        address: node.address,
        ageMs: node.lastSeenMsAgo,
        received: node.received,
        lost: node.lost,
        wireMismatch: wireMismatch(node.announce, gatewayWireHash),
        announce: node.announce,
        logModules: node.logModules,
    };
}

/** What changed between two tables, for the elements keyed by node id. */
export interface NodeDiff {
    readonly added: NodeView[];
    readonly removed: number[];
}

/**
 * The node table as the page sees it: the gateway's last NodeTable, the
 * gateway's own wire hash (from GatewayStatus) for the mismatch flag, and
 * the nodes heard from since the table arrived.
 */
export class NodeModel {
    private nodes = new Map<number, NodeView>();
    private gatewayWireHash = 0;
    private readonly listeners: ((nodes: NodeView[], diff: NodeDiff) => void)[] = [];

    /** Called after every change, with the whole list and what changed. */
    onChange(listener: (nodes: NodeView[], diff: NodeDiff) => void): void {
        this.listeners.push(listener);
    }

    /** The gateway's schema hash; a change re-evaluates every flag. */
    setGatewayWireHash(wireHash: number): void {
        if (wireHash === this.gatewayWireHash) {
            return;
        }
        this.gatewayWireHash = wireHash;
        const next = new Map<number, NodeView>();
        for (const node of this.nodes.values()) {
            next.set(node.id, { ...node, wireMismatch: wireMismatch(node.announce, wireHash) });
        }
        this.replace(next);
    }

    /** A fresh table from the gateway replaces everything. */
    applyTable(table: NodeTable): void {
        const next = new Map<number, NodeView>();
        for (const node of table.nodes) {
            next.set(node.id, toView(node, this.gatewayWireHash));
        }
        this.replace(next);
    }

    /**
     * A frame from a node: it is alive right now, whatever the last table
     * said. A node the table does not list yet gets a placeholder so its
     * streams have an owner until the next table names it.
     */
    noteFrame(src: number): void {
        const known = this.nodes.get(src);
        if (known) {
            if (known.ageMs !== 0) {
                this.nodes.set(src, { ...known, ageMs: 0 });
            }
            return;
        }
        const next = new Map(this.nodes);
        next.set(src, {
            id: src,
            kind: NodeKind.NODE_KIND_UNSPECIFIED,
            kindName: kindName(NodeKind.NODE_KIND_UNSPECIFIED),
            name: `node ${hexNodeId(src)}`,
            address: "",
            ageMs: 0,
            received: 0,
            lost: 0,
            wireMismatch: false,
            announce: undefined,
            logModules: [],
        });
        this.replace(next);
    }

    /** The link dropped: nothing is known any more. */
    clear(): void {
        this.replace(new Map());
    }

    get(id: number): NodeView | undefined {
        return this.nodes.get(id);
    }

    list(): NodeView[] {
        return [...this.nodes.values()];
    }

    /** The drones, the nodes that get a widget, in a stable order. */
    drones(): NodeView[] {
        return this.list()
            .filter(isDrone)
            .sort((a, b) => a.id - b.id);
    }

    private replace(next: Map<number, NodeView>): void {
        const diff = diffNodes(this.nodes, next);
        this.nodes = next;
        const list = this.list();
        for (const listener of this.listeners) {
            listener(list, diff);
        }
    }
}

/**
 * The widgets to create and to destroy between two tables: a node is added
 * when it appears or when its kind becomes known (an announce arriving
 * after a placeholder), removed when it leaves.
 */
export function diffNodes(before: Map<number, NodeView>, after: Map<number, NodeView>): NodeDiff {
    const added: NodeView[] = [];
    const removed: number[] = [];
    for (const [id, node] of after) {
        const old = before.get(id);
        if (old === undefined || old.kind !== node.kind) {
            added.push(node);
        }
    }
    for (const [id, old] of before) {
        const node = after.get(id);
        if (node === undefined || node.kind !== old.kind) {
            removed.push(id);
        }
    }
    return { added, removed };
}
