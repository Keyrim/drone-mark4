// The node model of the extension: what the gateway's NodeTable becomes on
// screen. Pure (no vscode, no socket) so the tests drive it directly; the
// tree providers only turn these rows into TreeItems.

import { type Node, type TransportHealth, type TransportNodeHealth, TransportVerdict } from "./gen/gateway_pb";
import { type Announce, NodeKind } from "./gen/mark4_pb";

/** A node id as every log line of the project prints it: 8 hex digits. */
export function hexNodeId(id: number): string {
    return (id >>> 0).toString(16).padStart(8, "0");
}

/** Kind names and their codicons; an unknown kind stays generic. */
const KINDS: Record<number, { name: string; icon: string }> = {
    [NodeKind.FIRMWARE]: { name: "firmware", icon: "circuit-board" },
    [NodeKind.DRONE_SIM]: { name: "drone_sim", icon: "vm" },
    [NodeKind.PLANT]: { name: "plant", icon: "globe" },
    [NodeKind.GATEWAY]: { name: "gateway", icon: "server" },
    [NodeKind.BATCH]: { name: "batch", icon: "beaker" },
    // The ESP32 relay: named here so it reads as itself even on a build
    // whose schema does not know the value yet.
    [6]: { name: "relay", icon: "radio-tower" },
    [7]: { name: "phone", icon: "device-mobile" },
};

export function kindName(kind: number): string {
    return KINDS[kind]?.name ?? (kind === NodeKind.NODE_KIND_UNSPECIFIED ? "unknown" : `kind ${kind}`);
}

export function kindIcon(kind: number): string {
    return KINDS[kind]?.icon ?? "question";
}

/** True when the node was built on another mark4.proto than the gateway. */
export function wireMismatch(announce: Announce | undefined, gatewayWireHash: number): boolean {
    return announce !== undefined && gatewayWireHash !== 0 && announce.wireHash !== gatewayWireHash;
}

/** A node is live while its last frame is recent; older reads as fading. */
export const LIVE_MS = 1500;

/**
 * Node ids of the drone_sim instances the extension starts. A node id is
 * self-assigned and random, so nothing would tie a node of the table back to
 * the task that runs it: the extension picks the id itself, out of a range
 * it recognises, and the instance number is what is left of it.
 */
export const SIM_NODE_ID_BASE = 0xd5000000;
const SIM_MAX_INSTANCES = 64;

export function simNodeId(instance: number): number {
    return SIM_NODE_ID_BASE + instance;
}

/** The instance behind a node id, undefined when the extension did not start it. */
export function simInstance(nodeId: number): number | undefined {
    const instance = (nodeId >>> 0) - SIM_NODE_ID_BASE;
    return instance >= 1 && instance <= SIM_MAX_INSTANCES ? instance : undefined;
}

/** The verdict word of the gateway, as the tooltip and the view say it. */
export function verdictName(verdict: TransportVerdict): string {
    switch (verdict) {
        case TransportVerdict.VERDICT_OK:
            return "ok";
        case TransportVerdict.VERDICT_DEGRADED:
            return "degraded";
        case TransportVerdict.VERDICT_BAD:
            return "bad";
        default:
            return "unknown";
    }
}

/** A loss (0..1) as a percentage with one decimal. */
function percent(ratio: number): string {
    return `${(ratio * 100).toFixed(1)} %`;
}

/** One word per flag the gateway raised on a node, in severity order. */
function healthFlags(health: TransportNodeHealth): string[] {
    const flags: [boolean, string][] = [
        [health.asymmetric, "asymmetric"],
        [health.requestsFailed, "requests-failed"],
        [health.fading, "fading"],
        [health.linkRefused, "refused"],
        [health.linkRxErrors, "rx-errors"],
        [health.churn, "churn"],
    ];
    return flags.filter(([raised]) => raised).map(([, word]) => word);
}

/** The transport line of a tooltip: what the gateway makes of that node. */
function transportLine(health: TransportNodeHealth | undefined): string {
    if (health === undefined) {
        return "transport: unknown";
    }
    const words = [
        `transport: ${verdictName(health.verdict)}`,
        `in loss ${percent(health.worstInLoss)}`,
        `out loss ${percent(health.worstOutLoss)}`,
        ...healthFlags(health),
    ];
    return words.join(", ");
}

/** One line of the nodes view, everything it shows already resolved. */
export interface NodeRow {
    readonly id: number;
    readonly hex: string;
    readonly kind: number;
    readonly kindName: string;
    readonly icon: string;
    /** Announce name, the kind name until the node has announced. */
    readonly name: string;
    readonly live: boolean;
    readonly mismatch: boolean;
    /** What the gateway makes of this node's transport, UNKNOWN until it says. */
    readonly verdict: TransportVerdict;
    readonly tooltip: string;
}

function announceLines(announce: Announce | undefined): string[] {
    if (announce === undefined) {
        return ["no announce yet"];
    }
    const built = announce.buildEpoch === 0 ? "unknown" : new Date(announce.buildEpoch * 1000).toISOString();
    return [
        `built ${built}`,
        `git ${announce.gitHash === "" ? "unknown" : announce.gitHash}`,
        `wire ${announce.wireHash.toString(16).padStart(8, "0")}`,
    ];
}

/** One row per node of a table, in the order the gateway published them. */
export function nodeRows(
    nodes: readonly Node[],
    gatewayWireHash: number,
    health: TransportHealth | undefined,
): NodeRow[] {
    return nodes.map((node) => {
        const nodeHealth = health?.nodes.find((entry) => entry.node === node.id);
        const kind = node.announce?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED;
        const word = kindName(kind);
        const mismatch = wireMismatch(node.announce, gatewayWireHash);
        const address = node.address === "" ? "this gateway" : `${node.address}:${node.port}`;
        const tooltip = [
            `${node.announce?.name || word} (${word} ${hexNodeId(node.id)})`,
            address,
            ...announceLines(node.announce),
            `last seen ${node.lastSeenMsAgo} ms ago`,
            transportLine(nodeHealth),
            ...(mismatch ? ["WIRE MISMATCH: built on another mark4.proto than the gateway"] : []),
        ].join("\n");
        return {
            id: node.id,
            hex: hexNodeId(node.id),
            kind,
            kindName: word,
            icon: kindIcon(kind),
            name: node.announce?.name || word,
            live: node.lastSeenMsAgo < LIVE_MS,
            mismatch,
            verdict: nodeHealth?.verdict ?? TransportVerdict.VERDICT_UNKNOWN,
            tooltip,
        };
    });
}

/** What changed between two node tables, in what the view draws. */
export interface RowChanges {
    /** Nodes appeared, disappeared or moved: only a whole redraw fits. */
    readonly structural: boolean;
    /** Ids of the lines that read differently. */
    readonly changed: number[];
}

/** True when two rows would draw the same. The tooltip is not compared: its
 * losses and its last-seen move on every table and must not redraw the
 * view (they are read one refresh late, which is what a tooltip is worth). */
function sameRow(left: NodeRow, right: NodeRow): boolean {
    return (
        left.id === right.id &&
        left.name === right.name &&
        left.kind === right.kind &&
        left.live === right.live &&
        left.mismatch === right.mismatch &&
        left.verdict === right.verdict
    );
}

/**
 * The redraw a new table needs. The gateway republishes the whole table once
 * a second and most of it never moves: comparing what is drawn is what stops
 * the view from blinking every second.
 */
export function diffNodeRows(previous: readonly NodeRow[], next: readonly NodeRow[]): RowChanges {
    if (previous.length !== next.length || previous.some((row, index) => row.id !== next[index]?.id)) {
        return { structural: true, changed: [] };
    }
    return {
        structural: false,
        changed: next.filter((row, index) => !sameRow(previous[index] as NodeRow, row)).map((row) => row.id),
    };
}
