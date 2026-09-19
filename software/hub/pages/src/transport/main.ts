/**
 * Transport page: the state of the wire itself, from every node at once.
 *
 * Every node reports what its transport and its messenger count; the gateway
 * gathers those views, turns the cumulative counters into rates over a
 * sliding window and judges the whole with one verdict per node. This page
 * draws what it publishes: the banner and the findings say what is wrong in
 * words, the graph says where, and the panel says how much for the node that
 * is selected.
 *
 * The reports only travel while a client asks for them, so the page marks
 * itself on every socket open and clears its mark when it goes away.
 */

import "@vscode-elements/elements/dist/vscode-badge/index.js";
import "@vscode-elements/elements/dist/vscode-checkbox/index.js";
import "@vscode-elements/elements/dist/vscode-icon/index.js";
import "@vscode-elements/elements/dist/vscode-split-layout/index.js";
import "@vscode-elements/elements/dist/vscode-table/index.js";
import "@vscode-elements/elements/dist/vscode-table-body/index.js";
import "@vscode-elements/elements/dist/vscode-table-cell/index.js";
import "@vscode-elements/elements/dist/vscode-table-header/index.js";
import "@vscode-elements/elements/dist/vscode-table-header-cell/index.js";
import "@vscode-elements/elements/dist/vscode-table-row/index.js";
import "@vscode-elements/elements/dist/vscode-tab-header/index.js";
import "@vscode-elements/elements/dist/vscode-tab-panel/index.js";
import "@vscode-elements/elements/dist/vscode-tabs/index.js";

import { create } from "@bufbuild/protobuf";

import { GatewayMessageSchema, type NodeTransport, type TransportHealth } from "../gen/gateway_pb";
import { GatewaySocket } from "../shared/gateway_socket";
import { hexNodeId } from "../shared/nodes";
import { Shell } from "../shared/shell";
import { TransportGraph, verdictClass, verdictWord } from "./graph";
import { findings, percent, rate, type Finding } from "./health";
import { HISTORY, TransportPanel, type PanelHistory } from "./panel";
import "./transport.css";

const socket = new GatewaySocket();
const shell = new Shell(socket);

/** The last view of every node a complete report is held of. */
const views = new Map<number, NodeTransport>();
/** The last minute of every node, for the panel's sparklines. */
const histories = new Map<number, { t: number[]; framesIn: number[]; loss: number[]; first: number }>();
let health: TransportHealth | null = null;
let dirty = true;

/* -------------------- the toolbar -------------------- */

// The node reports cost the wire something, so they are asked for while the
// page is watched and given back when it is not.
const live = document.createElement("vscode-checkbox");
live.setAttribute("label", "live reports");
live.checked = true;
live.addEventListener("change", () => {
    sendSubscribe(live.checked);
    if (!live.checked) {
        views.clear();
        histories.clear();
        dirty = true;
    }
});
shell.toolbar.appendChild(live);

/* -------------------- the banner -------------------- */

const banner = document.createElement("div");
banner.className = "tp-banner";
const bannerVerdict = document.createElement("vscode-badge");
bannerVerdict.className = "tp-verdict unknown";
bannerVerdict.textContent = verdictWord(undefined);
const bannerNodes = document.createElement("span");
const bannerWorst = document.createElement("span");
const bannerRate = document.createElement("span");
banner.append(bannerVerdict, bannerNodes, bannerWorst, bannerRate);

const findingsTable = document.createElement("vscode-table");
findingsTable.className = "tp-findings";
findingsTable.setAttribute("bordered-rows", "");
const findingsHead = document.createElement("vscode-table-header");
findingsHead.setAttribute("slot", "header");
for (const column of ["severity", "where", "what"]) {
    const cell = document.createElement("vscode-table-header-cell");
    cell.textContent = column;
    findingsHead.appendChild(cell);
}
const findingsBody = document.createElement("vscode-table-body");
findingsBody.setAttribute("slot", "body");
findingsTable.append(findingsHead, findingsBody);

/* -------------------- the two halves -------------------- */

const panel = new TransportPanel();
const graph = new TransportGraph(() => {
    dirty = true;
});

const split = document.createElement("vscode-split-layout");
// The component names a side-by-side divider a vertical split: the graph
// takes the left two thirds, the panel the right.
split.setAttribute("split", "vertical");
split.setAttribute("initial-handle-position", "65%");
const left = document.createElement("div");
left.className = "tp-half";
left.setAttribute("slot", "start");
left.appendChild(graph.root);
const right = document.createElement("div");
right.className = "tp-half";
right.setAttribute("slot", "end");
right.appendChild(panel.root);
split.append(left, right);

const head = document.createElement("div");
head.className = "tp-head";
head.append(banner, findingsTable);
shell.content.className = "content transport";
shell.content.append(head, split);

/* -------------------- the gateway -------------------- */

/** Asks the gateway for every node's reports, or gives them back. */
function sendSubscribe(on: boolean): void {
    socket.send(
        create(GatewayMessageSchema, { body: { case: "transportCommand", value: { subscribe: on } } })
    );
}

socket.onState((state) => {
    if (state === "open") {
        sendSubscribe(live.checked);
        return;
    }
    // The link is gone: so is everything it had published.
    views.clear();
    histories.clear();
    health = null;
    dirty = true;
});

socket.on("nodeTransport", (published) => {
    views.set(published.node, published);
    remember(published);
    dirty = true;
});

socket.on("transportHealth", (published) => {
    health = published;
    dirty = true;
});

shell.nodes.onChange((_nodes, diff) => {
    for (const id of diff.removed) {
        views.delete(id);
        histories.delete(id);
    }
    dirty = true;
});

// A page that goes away must not leave the nodes reporting to nobody.
for (const event of ["pagehide", "beforeunload"]) {
    addEventListener(event, () => sendSubscribe(false));
}

/** Keeps the last HISTORY samples of one node for its sparklines. */
function remember(published: NodeTransport): void {
    const now = Date.now() / 1000;
    const kept = histories.get(published.node) ?? { t: [], framesIn: [], loss: [], first: now };
    let framesIn = 0;
    for (const link of published.links) {
        framesIn += link.framesInPerS;
    }
    let loss = 0;
    for (const edge of published.edges) {
        loss = Math.max(loss, edge.loss);
    }
    kept.t.push(now - kept.first);
    kept.framesIn.push(framesIn);
    kept.loss.push(loss * 100);
    while (kept.t.length > HISTORY) {
        kept.t.shift();
        kept.framesIn.shift();
        kept.loss.shift();
    }
    histories.set(published.node, kept);
}

/* -------------------- painting -------------------- */

/** How a node is named on screen, wherever one line carries its id. */
function nameOf(id: number): string {
    return shell.nodes.get(id)?.name ?? `node ${hexNodeId(id)}`;
}

function paintBanner(): void {
    bannerVerdict.className = `tp-verdict ${verdictClass(health?.verdict)}`;
    bannerVerdict.textContent = verdictWord(health?.verdict);
    bannerNodes.textContent = `${health?.nodesReporting ?? 0} reporting / ${health?.nodesKnown ?? 0} nodes`;
    bannerWorst.textContent =
        health === null || health.worstObserver === 0
            ? "no loss measured"
            : `worst ${nameOf(health.worstObserver)} -> ${nameOf(health.worstPeer)} ${percent(health.worstLoss)}`;
    bannerRate.textContent = `${rate(health?.framesPerS ?? 0)} frames/s`;
}

function paintFindings(): void {
    const lines = findings({ health, views, name: nameOf });
    if (lines.length === 0) {
        findingsBody.replaceChildren(findingRow({ severity: "info", where: "", what: "nothing to report" }));
        return;
    }
    findingsBody.replaceChildren(...lines.map(findingRow));
}

function findingRow(finding: Finding): HTMLElement {
    const row = document.createElement("vscode-table-row");
    const severity = document.createElement("vscode-table-cell");
    const badge = document.createElement("vscode-badge");
    badge.className = `tp-verdict ${finding.severity === "info" ? "unknown" : finding.severity}`;
    badge.textContent = finding.severity;
    severity.appendChild(badge);
    const where = document.createElement("vscode-table-cell");
    where.textContent = finding.where;
    const what = document.createElement("vscode-table-cell");
    what.textContent = finding.what;
    row.append(severity, where, what);
    return row;
}

function paint(): void {
    const nodes = shell.nodes.list();
    paintBanner();
    paintFindings();
    graph.render({ nodes, views, health });
    const selected = graph.selected;
    const node = selected === null ? undefined : nodes.find((one) => one.id === selected);
    const kept = selected === null ? undefined : histories.get(selected);
    const history: PanelHistory | undefined =
        kept === undefined ? undefined : { t: kept.t, framesIn: kept.framesIn, loss: kept.loss };
    panel.render({
        node,
        view: selected === null ? undefined : views.get(selected),
        health: health?.nodes.find((one) => one.node === selected),
        views,
        history,
        name: nameOf,
    });
}

function frame(): void {
    requestAnimationFrame(frame);
    if (!dirty) {
        return;
    }
    dirty = false;
    paint();
}

requestAnimationFrame(frame);
