/**
 * The picture of the wire: one card per node, absolutely positioned over one
 * SVG layer that draws the buses, the stubs tying each card to its bus, and
 * the directed edges between the nodes that hear each other.
 *
 * An edge is what one node counts of one peer, so it is drawn from the
 * observer to the peer: a loss belongs to the direction that measured it.
 * Only the edges worth a color are drawn at rest; hovering or selecting a
 * card draws every edge it is an end of.
 *
 * Where the cards go is layout.ts; what the colors mean is health.ts.
 */

import { type NodeTransport, type TransportHealth, TransportVerdict } from "../gen/gateway_pb";
import { NodeKind } from "../gen/mark4_pb";
import { hexNodeId, type NodeView } from "../shared/nodes";
import {
    keepaliveOnly,
    linkWord,
    lossClass,
    percent,
    quiet,
    rate,
    windowCount,
    windowSpan,
    type LossClass,
} from "./health";
import { CARD_H, CARD_W, layoutGraph, PORT_Y, type Card, type Layout, type LayoutNode } from "./layout";

const SVG_NS = "http://www.w3.org/2000/svg";

/** How far outside a card an arrowhead stops [px]. */
const ARROW_GAP = 7;

/** The codicon of every kind, as the editor's node tree draws them. */
const KIND_ICONS: Record<number, string> = {
    [NodeKind.FIRMWARE]: "circuit-board",
    [NodeKind.DRONE_SIM]: "vm",
    [NodeKind.PLANT]: "globe",
    [NodeKind.GATEWAY]: "server",
    [NodeKind.BATCH]: "beaker",
    [NodeKind.RELAY]: "radio-tower",
    [NodeKind.PHONE]: "device-mobile",
};

/** The icon of a kind, generic for one nothing names. */
export function kindIcon(kind: NodeKind): string {
    return KIND_ICONS[kind] ?? "question";
}

/** The class a verdict wears, on a card dot as on a badge. */
export function verdictClass(verdict: TransportVerdict | undefined): string {
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

/** A verdict as the banner and the panel word it. */
export function verdictWord(verdict: TransportVerdict | undefined): string {
    switch (verdict) {
        case TransportVerdict.VERDICT_OK:
            return "OK";
        case TransportVerdict.VERDICT_DEGRADED:
            return "DEGRADED";
        case TransportVerdict.VERDICT_BAD:
            return "BAD";
        default:
            return "UNKNOWN";
    }
}

/** Everything the graph paints from. */
export interface GraphInput {
    readonly nodes: readonly NodeView[];
    /** One view per node a complete report is held of, by node id. */
    readonly views: ReadonlyMap<number, NodeTransport>;
    readonly health: TransportHealth | null;
}

export class TransportGraph {
    readonly root: HTMLElement;
    private readonly canvas: HTMLElement;
    private readonly wires: SVGSVGElement;
    private edgeLayer: SVGGElement;
    private input: GraphInput = { nodes: [], views: new Map(), health: null };
    private layout: Layout = { media: [], cards: [], buses: [], stubs: [], width: 0, height: 0 };
    private names = new Map<number, string>();
    private hovered: number | null = null;
    private chosen: number | null = null;

    /** @param onSelect told which card is selected, null when none is */
    constructor(private readonly onSelect: (id: number | null) => void) {
        this.root = document.createElement("div");
        this.root.className = "tp-graph";
        this.canvas = document.createElement("div");
        this.canvas.className = "tp-canvas";
        this.wires = document.createElementNS(SVG_NS, "svg");
        this.wires.setAttribute("class", "tp-wires");
        this.wires.appendChild(arrowDefs());
        this.edgeLayer = document.createElementNS(SVG_NS, "g");
        this.canvas.appendChild(this.wires);
        this.root.appendChild(this.canvas);
        this.root.addEventListener("click", (event) => {
            if (event.target === this.root || event.target === this.canvas) {
                this.select(null);
            }
        });
    }

    /** The selected node, null when the graph shows no selection. */
    get selected(): number | null {
        return this.chosen;
    }

    /** Repaints everything: the cards, the buses and the edges. */
    render(input: GraphInput): void {
        this.input = input;
        this.names = new Map(input.nodes.map((node) => [node.id, node.name]));
        this.layout = layoutGraph(input.nodes.map((node) => toLayoutNode(node, input)));
        if (this.chosen !== null && !input.nodes.some((node) => node.id === this.chosen)) {
            this.select(null);
        }

        this.canvas.style.width = `${this.layout.width}px`;
        this.canvas.style.height = `${this.layout.height}px`;
        this.wires.setAttribute("width", String(this.layout.width));
        this.wires.setAttribute("height", String(this.layout.height));
        this.wires.setAttribute("viewBox", `0 0 ${this.layout.width} ${this.layout.height}`);

        this.canvas.querySelectorAll(".tp-card").forEach((card) => card.remove());
        for (const card of this.layout.cards) {
            this.canvas.appendChild(this.buildCard(card));
        }
        this.paintWires();
    }

    /** Selects one card, or none, and tells the page. */
    select(id: number | null): void {
        if (id === this.chosen) {
            return;
        }
        this.chosen = id;
        for (const element of this.canvas.querySelectorAll(".tp-card")) {
            element.classList.toggle("selected", Number(element.getAttribute("data-node")) === id);
        }
        this.paintWires();
        this.onSelect(id);
    }

    /** One card: who the node is, how it fares, and what its links carry. */
    private buildCard(placed: Card): HTMLElement {
        const node = this.input.nodes.find((one) => one.id === placed.id);
        const view = this.input.views.get(placed.id);
        const health = this.input.health?.nodes.find((one) => one.node === placed.id);

        const card = document.createElement("div");
        card.className = placed.id === this.chosen ? "tp-card selected" : "tp-card";
        card.dataset["node"] = String(placed.id);
        card.style.left = `${placed.x}px`;
        card.style.top = `${placed.y}px`;
        card.style.width = `${CARD_W}px`;
        card.style.height = `${CARD_H}px`;

        const head = document.createElement("div");
        head.className = "tp-card-head";
        const icon = document.createElement("vscode-icon");
        icon.setAttribute("name", kindIcon(node?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED));
        const name = document.createElement("span");
        name.className = "tp-card-name";
        name.textContent = node?.name ?? `node ${hexNodeId(placed.id)}`;
        const dot = document.createElement("span");
        dot.className = `tp-dot ${verdictClass(health?.verdict)}`;
        dot.title = `transport ${verdictWord(health?.verdict).toLowerCase()}`;
        head.append(icon, name, dot);

        const id = document.createElement("div");
        id.className = "tp-card-id";
        id.textContent = `${node?.kindName ?? "unknown"} ${hexNodeId(placed.id)}`;

        const ports = document.createElement("div");
        ports.className = "tp-ports";
        if (view === undefined) {
            ports.appendChild(chip("no report", "muted"));
        } else {
            (view.report?.links ?? []).forEach((link, index) => {
                const measured = view.links[index];
                const text =
                    measured === undefined
                        ? linkWord(link.kind)
                        : `${linkWord(link.kind)} ${rate(measured.framesInPerS)}/${rate(measured.framesOutPerS)} fps`;
                ports.appendChild(chip(text, ""));
            });
        }
        if (placed.note !== "") {
            ports.appendChild(chip(placed.note, "muted"));
        }

        card.append(head, id, ports);
        card.addEventListener("click", (event) => {
            event.stopPropagation();
            this.select(placed.id === this.chosen ? null : placed.id);
        });
        card.addEventListener("pointerenter", () => {
            this.hovered = placed.id;
            this.paintWires();
        });
        card.addEventListener("pointerleave", () => {
            this.hovered = null;
            this.paintWires();
        });
        return card;
    }

    /** The whole SVG layer: the buses, the stubs, then the edges over them. */
    private paintWires(): void {
        this.edgeLayer.remove();
        for (const line of [...this.wires.querySelectorAll(".tp-bus, .tp-stub")]) {
            line.remove();
        }
        for (const bus of this.layout.buses) {
            const line = document.createElementNS(SVG_NS, "line");
            line.setAttribute("class", "tp-bus");
            line.setAttribute("x1", String(bus.x));
            line.setAttribute("y1", String(bus.y0));
            line.setAttribute("x2", String(bus.x));
            line.setAttribute("y2", String(bus.y1));
            line.appendChild(titleOf(`${linkWord(bus.kind)}: ${this.layout.media[bus.medium]?.members.length ?? 0} nodes`));
            this.wires.appendChild(line);
        }
        for (const stub of this.layout.stubs) {
            const line = document.createElementNS(SVG_NS, "line");
            line.setAttribute("class", "tp-stub");
            line.setAttribute("x1", String(stub.x0));
            line.setAttribute("y1", String(stub.y));
            line.setAttribute("x2", String(stub.x1));
            line.setAttribute("y2", String(stub.y));
            this.wires.appendChild(line);
        }
        this.edgeLayer = document.createElementNS(SVG_NS, "g");
        this.wires.appendChild(this.edgeLayer);
        this.paintEdges();
    }

    /** The edges: the loud ones always, the rest with the card they touch. */
    private paintEdges(): void {
        const cards = new Map(this.layout.cards.map((card) => [card.id, card]));
        // Both ends of what a person is looking at: the card under the
        // pointer and the one that stays selected under it.
        const focus = [this.hovered, this.chosen].filter((one): one is number => one !== null);
        for (const view of this.input.views.values()) {
            const from = cards.get(view.node);
            if (from === undefined) {
                continue;
            }
            for (const edge of view.edges) {
                const to = cards.get(edge.peer);
                const grade = lossClass(edge, view.windowMs);
                const touched = focus.includes(view.node) || focus.includes(edge.peer);
                if (to === undefined || (!touched && grade !== "degraded" && grade !== "bad")) {
                    continue;
                }
                const oneSided = !this.input.views.has(edge.peer);
                this.edgeLayer.appendChild(
                    this.buildEdge(from, to, grade, oneSided, {
                        observer: view.node,
                        peer: edge.peer,
                        text:
                            `${this.name(view.node)} -> ${this.name(edge.peer)}: ` +
                            `loss ${percent(edge.loss)} (${windowCount(edge)} ${windowSpan(view.windowMs)}), ` +
                            `${rate(edge.rxPerS)} fps, dup ${rate(edge.duplicatesPerS)}/s, ` +
                            `hops ${edge.hops}, age ${edge.ageMs} ms` +
                            (quiet(edge) ? `, ${keepaliveOnly(edge, view.windowMs)}` : ""),
                    })
                );
            }
        }
    }

    /** One arrow from the observer's card to the peer's. */
    private buildEdge(
        from: Card,
        to: Card,
        grade: LossClass,
        oneSided: boolean,
        about: { observer: number; peer: number; text: string }
    ): SVGElement {
        const start = center(from);
        const end = center(to);
        const tail = border(start, end, CARD_W / 2, CARD_H / 2);
        const head = border(end, start, CARD_W / 2 + ARROW_GAP, CARD_H / 2 + ARROW_GAP);
        const line = document.createElementNS(SVG_NS, "line");
        const classes = ["tp-edge", grade];
        if (oneSided) {
            classes.push("one-sided");
        }
        if (about.observer === this.chosen || about.peer === this.chosen) {
            classes.push("chosen");
        }
        line.setAttribute("class", classes.join(" "));
        line.setAttribute("x1", String(tail.x));
        line.setAttribute("y1", String(tail.y));
        line.setAttribute("x2", String(head.x));
        line.setAttribute("y2", String(head.y));
        line.setAttribute("marker-end", `url(#tp-arrow-${grade})`);
        line.appendChild(titleOf(about.text));
        return line;
    }

    private name(id: number): string {
        return this.names.get(id) ?? `node ${hexNodeId(id)}`;
    }
}

/** What layout.ts needs of one node: its links, its direct peers, its hops. */
function toLayoutNode(node: NodeView, input: GraphInput): LayoutNode {
    const view = input.views.get(node.id);
    const gateway = input.nodes.find((one) => one.kind === NodeKind.GATEWAY);
    const seen = gateway === undefined ? undefined : input.views.get(gateway.id);
    const fromGateway = seen?.edges.find((edge) => edge.peer === node.id);
    return {
        id: node.id,
        kind: node.kind,
        links: (view?.report?.links ?? []).map((link) => link.kind),
        peers: (view?.edges ?? []).map((edge) => ({ id: edge.peer, link: edge.link, hops: edge.hops })),
        hops: gateway?.id === node.id ? 0 : (fromGateway?.hops ?? -1),
    };
}

/** One arrowhead per loss class: a marker cannot read the line's stroke. */
function arrowDefs(): SVGDefsElement {
    const defs = document.createElementNS(SVG_NS, "defs");
    for (const grade of ["idle", "quiet", "ok", "degraded", "bad"]) {
        const marker = document.createElementNS(SVG_NS, "marker");
        marker.setAttribute("id", `tp-arrow-${grade}`);
        marker.setAttribute("viewBox", "0 0 8 8");
        marker.setAttribute("refX", "7");
        marker.setAttribute("refY", "4");
        marker.setAttribute("markerWidth", "6");
        marker.setAttribute("markerHeight", "6");
        marker.setAttribute("orient", "auto-start-reverse");
        const path = document.createElementNS(SVG_NS, "path");
        path.setAttribute("class", `tp-arrow ${grade}`);
        path.setAttribute("d", "M 0 0 L 8 4 L 0 8 z");
        marker.appendChild(path);
        defs.appendChild(marker);
    }
    return defs;
}

function titleOf(text: string): SVGTitleElement {
    const title = document.createElementNS(SVG_NS, "title");
    title.textContent = text;
    return title;
}

function chip(text: string, extra: string): HTMLElement {
    const element = document.createElement("span");
    element.className = extra === "" ? "tp-port" : `tp-port ${extra}`;
    element.textContent = text;
    return element;
}

function center(card: Card): { x: number; y: number } {
    return { x: card.x + CARD_W / 2, y: card.y + PORT_Y / 2 + CARD_H / 4 };
}

/** Where the segment towards a point leaves a box of that half size. */
function border(from: { x: number; y: number }, to: { x: number; y: number }, halfW: number, halfH: number) {
    const dx = to.x - from.x;
    const dy = to.y - from.y;
    if (dx === 0 && dy === 0) {
        return from;
    }
    const scaleX = dx === 0 ? Number.POSITIVE_INFINITY : halfW / Math.abs(dx);
    const scaleY = dy === 0 ? Number.POSITIVE_INFINITY : halfH / Math.abs(dy);
    const scale = Math.min(scaleX, scaleY);
    return { x: from.x + dx * scale, y: from.y + dy * scale };
}
