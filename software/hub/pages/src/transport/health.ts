/**
 * The verdict in words: the findings the transport page lists under its
 * banner, read from the gateway's health message and the views it judged.
 *
 * The gateway says what is wrong as flags and numbers; a finding is the
 * sentence a person reads, with the two nodes or the link it is about. One
 * line per edge that loses frames, per fading edge, per asymmetric pair, per
 * link that refused frames or could not read them, per node that gave up on
 * requests or that saw its peers churn, and one per node that does not
 * report at all.
 *
 * Pure: no DOM, no socket. The page sorts them into its table.
 */

import { type NodeTransport, type TransportEdge, type TransportHealth } from "../gen/gateway_pb";
import { LinkKind } from "../gen/mark4_pb";

/**
 * Loss on one edge above this is degraded. The gateway judges with the same
 * four numbers (LOSS_DEGRADED, LOSS_BAD, FADING_MS and LOSS_MIN_FRAMES of
 * software/hub/include/hub/transport_health.hpp); they are repeated here to
 * color and to word what it already decided, never to decide again.
 */
export const LOSS_DEGRADED = 0.01;

/** Loss on one edge above this is bad: a tenth of what is sent is gone. */
export const LOSS_BAD = 0.1;

/** An edge whose last frame is older than this is fading [ms]. */
export const FADING_MS = 1500;

/**
 * An edge whose window holds fewer frames than this is quiet: a percentage
 * over a handful of frames is not a measurement, so it is not judged on its
 * loss. A keepalive alone fills ten frames of a ten second window, which is
 * what such an edge carries.
 */
export const LOSS_MIN_FRAMES = 20;

/** How a finding reads: what it costs, worst first. */
export type Severity = "bad" | "degraded" | "info";

/** One line of the findings table. */
export interface Finding {
    readonly severity: Severity;
    /** The nodes or the link it is about. */
    readonly where: string;
    /** What happened to them. */
    readonly what: string;
}

/** What the findings are built from: the last of everything the page holds. */
export interface HealthInput {
    readonly health: TransportHealth | null;
    /** One view per node a complete report is held of, by node id. */
    readonly views: ReadonlyMap<number, NodeTransport>;
    /** How a node is named on screen. */
    readonly name: (id: number) => string;
}

/** A loss class, the same ladder the gateway judged with. */
export type LossClass = "idle" | "quiet" | "ok" | "degraded" | "bad";

/**
 * Where one edge sits on the ladder; idle while no window was measured, and
 * quiet while the window holds too few frames to read a percentage from.
 */
export function lossClass(edge: TransportEdge, windowMs: number): LossClass {
    if (windowMs === 0) {
        return "idle";
    }
    if (quiet(edge)) {
        return "quiet";
    }
    if (edge.loss > LOSS_BAD) {
        return "bad";
    }
    if (edge.loss > LOSS_DEGRADED) {
        return "degraded";
    }
    return "ok";
}

/**
 * True while the window of one edge holds fewer than LOSS_MIN_FRAMES
 * frames: what it carries is the keepalive and little else, so its loss says
 * nothing and the page words it as keepalive only.
 */
export function quiet(edge: TransportEdge): boolean {
    return edge.windowReceived + edge.windowLost < LOSS_MIN_FRAMES;
}

/** A ratio as a percentage, one decimal while it is small. */
export function percent(ratio: number): string {
    return `${(ratio * 100).toFixed(ratio < 0.1 ? 1 : 0)} %`;
}

/** What a percentage was counted on: what the window lost of what it held. */
export function windowCount(edge: TransportEdge): string {
    return `${edge.windowLost}/${edge.windowReceived + edge.windowLost}`;
}

/** The span every count of a window is read over, in whole seconds. */
export function windowSpan(windowMs: number): string {
    return `over ${Math.round(windowMs / 1000)} s`;
}

/** What a quiet edge carried, in words: the frames and the span. */
export function keepaliveOnly(edge: TransportEdge, windowMs: number): string {
    return `keepalive only (${edge.windowReceived + edge.windowLost} frames ${windowSpan(windowMs)})`;
}

/** A rate with one decimal, as every port chip and every edge prints one. */
export function rate(value: number): string {
    return value.toFixed(1);
}

/** A link medium as one word. */
export function linkWord(kind: LinkKind): string {
    if (kind === LinkKind.LINK_UART) {
        return "uart";
    }
    if (kind === LinkKind.LINK_UDP) {
        return "udp";
    }
    return "link";
}

const RANK: Record<Severity, number> = { bad: 0, degraded: 1, info: 2 };

interface Weighted {
    readonly finding: Finding;
    /** Sorts the findings of one severity: the loudest first. */
    readonly weight: number;
}

/**
 * Every finding the last messages carry, worst first. An empty list is a
 * healthy wire: the banner says so on its own.
 */
export function findings(input: HealthInput): Finding[] {
    const lines: Weighted[] = [];
    for (const view of input.views.values()) {
        edgeFindings(input, view, lines);
        linkFindings(input, view, lines);
    }
    nodeFindings(input, lines);
    lines.sort(
        (a, b) =>
            RANK[a.finding.severity] - RANK[b.finding.severity] ||
            b.weight - a.weight ||
            a.finding.where.localeCompare(b.finding.where)
    );
    return lines.map((line) => line.finding);
}

/** What one node's own edges say: the losses, then the silences. */
function edgeFindings(input: HealthInput, view: NodeTransport, into: Weighted[]): void {
    const observer = input.name(view.node);
    for (const edge of view.edges) {
        const where = `${observer} -> ${input.name(edge.peer)}`;
        const grade = lossClass(edge, view.windowMs);
        if (grade === "bad" || grade === "degraded") {
            into.push({
                finding: {
                    severity: grade,
                    where,
                    what: `loses ${percent(edge.loss)} (${windowCount(edge)} ${windowSpan(view.windowMs)})`,
                },
                weight: edge.loss,
            });
        }
        if (edge.ageMs > FADING_MS) {
            into.push({
                finding: {
                    severity: "degraded",
                    where,
                    what: `fading: last frame ${edge.ageMs} ms ago`,
                },
                weight: edge.ageMs / 1000,
            });
        }
    }
}

/** What one node's links say: what they would not take, what they misread. */
function linkFindings(input: HealthInput, view: NodeTransport, into: Weighted[]): void {
    const links = view.report?.links ?? [];
    view.links.forEach((measured, index) => {
        const medium = links[index];
        const where = `${input.name(view.node)} ${linkWord(medium?.kind ?? LinkKind.LINK_KIND_UNSPECIFIED)}`;
        if (measured.refused > 0) {
            const full = medium?.kind === LinkKind.LINK_UART ? ", ring full" : "";
            into.push({
                finding: {
                    severity: "degraded",
                    where,
                    what: `${measured.refused} frames refused${full}`,
                },
                weight: measured.refused,
            });
        }
        if (measured.rxErrors > 0) {
            into.push({
                finding: {
                    severity: "degraded",
                    where,
                    what: `${measured.rxErrors} frames received broken`,
                },
                weight: measured.rxErrors,
            });
        }
    });
}

/** What the gateway said of each node: the flags it raised, then the silent ones. */
function nodeFindings(input: HealthInput, into: Weighted[]): void {
    for (const node of input.health?.nodes ?? []) {
        const where = input.name(node.node);
        const view = input.views.get(node.node);
        if (node.asymmetric) {
            for (const other of deafTowards(input, node.node)) {
                into.push({
                    finding: {
                        severity: "bad",
                        where,
                        what: `${input.name(other)} hears ${where}, ${where} does not hear ${input.name(other)}`,
                    },
                    weight: 1,
                });
            }
        }
        const window = view?.window;
        if (node.requestsFailed && window !== undefined) {
            into.push({
                finding: {
                    severity: "bad",
                    where,
                    what: `${window.failed} requests given up on`,
                },
                weight: window.failed,
            });
        }
        if (node.churn && window !== undefined) {
            into.push({
                finding: {
                    severity: "degraded",
                    where,
                    what: `${window.expired} peers expired, ${window.restarted} seen restarting`,
                },
                weight: window.expired + window.restarted,
            });
        }
        if (!node.reporting) {
            into.push({
                finding: { severity: "info", where, what: "does not report: inbound only" },
                weight: 0,
            });
        }
    }
}

/** The reporting nodes that list this one while it lists none of them back. */
function deafTowards(input: HealthInput, nodeId: number): number[] {
    const own = input.views.get(nodeId);
    if (own === undefined) {
        return [];
    }
    const heard = new Set(own.edges.map((edge) => edge.peer));
    const deaf: number[] = [];
    for (const other of input.views.values()) {
        if (other.node !== nodeId && !heard.has(other.node) && other.edges.some((edge) => edge.peer === nodeId)) {
            deaf.push(other.node);
        }
    }
    return deaf;
}
