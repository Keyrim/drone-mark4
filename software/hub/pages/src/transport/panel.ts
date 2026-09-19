/**
 * The panel beside the graph: everything one node reports.
 *
 * Three tables, one per question a person asks of a node - what its links
 * carry, what it hears from each peer, and what its messenger did with all
 * of it - plus two sparklines of the last minute. A node that does not
 * report has none of that, so its panel shows what the others hear of it
 * and nothing else.
 *
 * Every number is the last report as the gateway derived it: the panel
 * computes nothing but the serial utilization, which is a ratio to a baud
 * rate no message carries.
 */

import uPlot from "uplot";

import { type NodeTransport, type TransportNodeHealth } from "../gen/gateway_pb";
import { LinkKind } from "../gen/mark4_pb";
import { cssVar } from "../lanes/lanes";
import { hexNodeId, type NodeView } from "../shared/nodes";
import { kindIcon, verdictClass, verdictWord } from "./graph";
import { linkWord, percent, rate } from "./health";

/** What one serial line carries [bytes/s]: 921600 baud, 10 bits a byte. */
export const UART_BYTES_PER_S = 92160;

/** How many samples of a node the sparklines keep. */
export const HISTORY = 60;

/** Height of one sparkline [px]. */
const SPARK_H = 54;

/** What the sparklines are drawn from: one point per report of one node. */
export interface PanelHistory {
    /** Seconds since the first sample held of that node. */
    readonly t: readonly number[];
    /** Frames in per second, summed over the node's links. */
    readonly framesIn: readonly number[];
    /** Worst loss over the edges the node observes, as a percentage. */
    readonly loss: readonly number[];
}

/** What the panel paints from. */
export interface PanelInput {
    readonly node: NodeView | undefined;
    readonly view: NodeTransport | undefined;
    readonly health: TransportNodeHealth | undefined;
    /** Every view held, to find what the others hear of a silent node. */
    readonly views: ReadonlyMap<number, NodeTransport>;
    readonly history: PanelHistory | undefined;
    readonly name: (id: number) => string;
}

export class TransportPanel {
    readonly root: HTMLElement;
    private readonly head: HTMLElement;
    private readonly body: HTMLElement;
    /**
     * What the body was built for: the node, and whether it reports. The
     * empty string is the shape of no selection, so nothing starts there.
     */
    private shape = "none";
    private readonly bodies = new Map<string, HTMLElement>();
    private spark: Sparklines | null = null;

    constructor() {
        this.root = document.createElement("div");
        this.root.className = "tp-panel";
        this.head = document.createElement("div");
        this.head.className = "tp-panel-head";
        this.body = document.createElement("div");
        this.body.className = "tp-panel-body";
        this.root.append(this.head, this.body);
    }

    /** Paints one node, or the invitation to pick one. */
    render(input: PanelInput): void {
        const shape = input.node === undefined ? "" : `${input.node.id}:${input.view !== undefined}`;
        if (shape !== this.shape) {
            this.shape = shape;
            this.build(input);
        }
        this.paintHead(input);
        this.paintTables(input);
        this.paintSpark(input);
    }

    /** The tabs and the tables the current node needs, once. */
    private build(input: PanelInput): void {
        this.bodies.clear();
        this.spark?.destroy();
        this.spark = null;
        this.body.replaceChildren();
        if (input.node === undefined) {
            const note = document.createElement("div");
            note.className = "tp-note";
            note.textContent = "select a node";
            this.body.appendChild(note);
            return;
        }

        const tabs = document.createElement("vscode-tabs");
        if (input.view === undefined) {
            // Nothing of its own: only what the others hear of it.
            this.addTab(tabs, "seen by", "peers", [
                "observer",
                "link",
                "hops",
                "rx/s",
                "loss",
                "dup/s",
                "age ms",
            ]);
        } else {
            this.addTab(tabs, "links", "links", [
                "#",
                "kind",
                "frames in/s",
                "bytes in/s",
                "frames out/s",
                "bytes out/s",
                "refused",
                "rx errors",
                "use",
            ]);
            this.addTab(tabs, "peers", "peers", ["peer", "link", "hops", "rx/s", "loss", "dup/s", "age ms"]);
            this.addTab(tabs, "messenger", "counters", ["counter", "total", "window"]);
        }
        this.body.appendChild(tabs);

        this.spark = new Sparklines();
        this.body.appendChild(this.spark.root);
    }

    /** One tab of the panel: its header, its table, its body to fill. */
    private addTab(tabs: HTMLElement, title: string, slot: string, columns: readonly string[]): void {
        const header = document.createElement("vscode-tab-header");
        header.setAttribute("slot", "header");
        header.textContent = title;
        const panel = document.createElement("vscode-tab-panel");
        const table = document.createElement("vscode-table");
        table.setAttribute("bordered-rows", "");
        table.setAttribute("zebra", "");
        const head = document.createElement("vscode-table-header");
        head.setAttribute("slot", "header");
        for (const column of columns) {
            const cell = document.createElement("vscode-table-header-cell");
            cell.textContent = column;
            head.appendChild(cell);
        }
        const body = document.createElement("vscode-table-body");
        body.setAttribute("slot", "body");
        table.append(head, body);
        panel.appendChild(table);
        tabs.append(header, panel);
        this.bodies.set(slot, body);
    }

    /** Who the node is and how the gateway judges it. */
    private paintHead(input: PanelInput): void {
        this.head.replaceChildren();
        if (input.node === undefined) {
            return;
        }
        const icon = document.createElement("vscode-icon");
        icon.setAttribute("name", kindIcon(input.node.kind));
        const name = document.createElement("span");
        name.className = "tp-panel-name";
        name.textContent = input.node.name;
        const id = document.createElement("span");
        id.className = "tp-panel-id";
        id.textContent = `${input.node.kindName} ${hexNodeId(input.node.id)}`;
        const badge = document.createElement("vscode-badge");
        badge.className = `tp-verdict ${verdictClass(input.health?.verdict)}`;
        badge.textContent = verdictWord(input.health?.verdict);
        this.head.append(icon, name, id, badge);
    }

    /** Every table of the current tabs, refilled from the last report. */
    private paintTables(input: PanelInput): void {
        const links = this.bodies.get("links");
        if (links !== undefined) {
            links.replaceChildren(...this.linkRows(input));
        }
        const peers = this.bodies.get("peers");
        if (peers !== undefined) {
            peers.replaceChildren(...this.peerRows(input));
        }
        const counters = this.bodies.get("counters");
        if (counters !== undefined) {
            counters.replaceChildren(...counterRows(input));
        }
    }

    /** One row per link: what it carried, and what a serial line has left. */
    private linkRows(input: PanelInput): HTMLElement[] {
        const view = input.view;
        if (view === undefined) {
            return [];
        }
        return (view.report?.links ?? []).map((link, index) => {
            const measured = view.links[index];
            const bytes = (measured?.bytesInPerS ?? 0) + (measured?.bytesOutPerS ?? 0);
            const use = link.kind === LinkKind.LINK_UART ? percent(bytes / UART_BYTES_PER_S) : "";
            return row([
                String(index),
                linkWord(link.kind),
                rate(measured?.framesInPerS ?? 0),
                rate(measured?.bytesInPerS ?? 0),
                rate(measured?.framesOutPerS ?? 0),
                rate(measured?.bytesOutPerS ?? 0),
                String(measured?.refused ?? 0),
                String(measured?.rxErrors ?? 0),
                use,
            ]);
        });
    }

    /** One row per edge: the node's own when it reports, the others' when not. */
    private peerRows(input: PanelInput): HTMLElement[] {
        if (input.node === undefined) {
            return [];
        }
        const nodeId = input.node.id;
        if (input.view !== undefined) {
            return input.view.edges.map((edge) =>
                row([
                    `${input.name(edge.peer)} ${hexNodeId(edge.peer)}`,
                    String(edge.link),
                    String(edge.hops),
                    rate(edge.rxPerS),
                    percent(edge.loss),
                    rate(edge.duplicatesPerS),
                    String(edge.ageMs),
                ])
            );
        }
        const rows: HTMLElement[] = [];
        for (const other of input.views.values()) {
            for (const edge of other.edges) {
                if (edge.peer !== nodeId) {
                    continue;
                }
                rows.push(
                    row([
                        `${input.name(other.node)} ${hexNodeId(other.node)}`,
                        String(edge.link),
                        String(edge.hops),
                        rate(edge.rxPerS),
                        percent(edge.loss),
                        rate(edge.duplicatesPerS),
                        String(edge.ageMs),
                    ])
                );
            }
        }
        return rows;
    }

    /** The two curves of the last minute, redrawn from the node's history. */
    private paintSpark(input: PanelInput): void {
        if (this.spark === null) {
            return;
        }
        const history = input.history;
        if (input.view === undefined || history === undefined || history.t.length === 0) {
            this.spark.clear();
            return;
        }
        this.spark.setData(history);
    }
}

/** The counters of the report, cumulative and over the window. */
function counterRows(input: PanelInput): HTMLElement[] {
    const report = input.view?.report;
    const window = input.view?.window;
    if (report === undefined) {
        return [];
    }
    const lines: [string, number, number | null][] = [
        ["sent", report.sent, window?.sent ?? null],
        ["refused", report.refused, window?.refused ?? null],
        ["dropped", report.dropped, window?.dropped ?? null],
        ["relayed", report.relayed, window?.relayed ?? null],
        ["expired", report.expired, window?.expired ?? null],
        ["restarted", report.restarted, window?.restarted ?? null],
        ["undecodable", report.undecodable, window?.undecodable ?? null],
        ["unhandled", report.unhandled, window?.unhandled ?? null],
        ["requests", report.requests, window?.requests ?? null],
        ["resent", report.resent, window?.resent ?? null],
        ["completed", report.completed, null],
        ["failed", report.failed, window?.failed ?? null],
        ["unmatched acks", report.unmatchedAcks, null],
    ];
    return lines.map((line) => row([line[0], String(line[1]), line[2] === null ? "" : String(line[2])]));
}

/** One table row of plain cells. */
function row(cells: readonly string[]): HTMLElement {
    const element = document.createElement("vscode-table-row");
    for (const text of cells) {
        const cell = document.createElement("vscode-table-cell");
        cell.textContent = text;
        element.appendChild(cell);
    }
    return element;
}

/**
 * The two measures of the last minute, one chart each: frames and loss do
 * not share a scale, and two scales on one plot is a chart nobody can read.
 */
class Sparklines {
    readonly root: HTMLElement;
    private readonly charts: Spark[];
    private readonly observer: ResizeObserver;

    constructor() {
        this.root = document.createElement("div");
        this.root.className = "tp-sparks";
        this.charts = [
            new Spark("frames in", "/s", cssVar("--vscode-charts-blue", "#3794ff")),
            new Spark("worst in loss", " %", cssVar("--vscode-charts-red", "#f14c4c")),
        ];
        for (const chart of this.charts) {
            this.root.appendChild(chart.root);
        }
        this.observer = new ResizeObserver(() => {
            for (const chart of this.charts) {
                chart.resize(this.root.clientWidth);
            }
        });
        this.observer.observe(this.root);
    }

    setData(history: PanelHistory): void {
        const t = [...history.t];
        this.charts[0]?.setData(t, [...history.framesIn]);
        this.charts[1]?.setData(t, [...history.loss]);
    }

    clear(): void {
        for (const chart of this.charts) {
            chart.setData([], []);
        }
    }

    destroy(): void {
        this.observer.disconnect();
        for (const chart of this.charts) {
            chart.destroy();
        }
        this.root.remove();
    }
}

/** One sparkline: its name, its last value, and the curve under them. */
class Spark {
    readonly root: HTMLElement;
    private readonly plotEl: HTMLElement;
    private readonly value: HTMLElement;
    private plot: uPlot | null = null;

    constructor(
        title: string,
        private readonly unit: string,
        private readonly stroke: string
    ) {
        this.root = document.createElement("div");
        this.root.className = "tp-spark";
        const head = document.createElement("div");
        head.className = "tp-spark-head";
        const name = document.createElement("span");
        name.textContent = title;
        this.value = document.createElement("b");
        head.append(name, this.value);
        this.plotEl = document.createElement("div");
        this.root.append(head, this.plotEl);
    }

    setData(t: number[], v: number[]): void {
        const last = v[v.length - 1];
        this.value.textContent = last === undefined ? "" : `${last.toFixed(1)}${this.unit}`;
        const data = [t, v] as unknown as uPlot.AlignedData;
        if (this.plot === null) {
            this.plot = new uPlot(this.options(this.plotEl.clientWidth), data, this.plotEl);
            return;
        }
        this.plot.setData(data);
    }

    resize(width: number): void {
        this.plot?.setSize({ width: Math.max(80, width - 16), height: SPARK_H });
    }

    destroy(): void {
        this.plot?.destroy();
        this.plot = null;
    }

    private options(width: number): uPlot.Options {
        return {
            width: Math.max(80, width),
            height: SPARK_H,
            padding: [4, 4, 0, 0],
            legend: { show: false },
            cursor: { points: { show: false }, drag: { x: false, y: false } },
            scales: { x: { time: false }, y: { auto: true } },
            axes: [{ show: false }, { show: false }],
            series: [{}, { stroke: this.stroke, width: 2, points: { show: false }, spanGaps: false }],
        };
    }
}
