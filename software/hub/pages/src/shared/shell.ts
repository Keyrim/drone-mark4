/**
 * Page shell shared by the two hub windows: the top bar opens with one chip
 * per node the gateway hears (kind, name, node id, age, and the wire
 * mismatch flag), then the gateway counters, the connection dot, and the
 * toast strip every page reports through.
 *
 * There is no in-page navigation: control and plots are two windows meant
 * to live on two screens, each with its own websocket to the gateway.
 */

import { type GatewayMessage } from "../gen/gateway_pb";
import { LogLevel } from "../gen/mark4_pb";
import { type Ack, type GatewaySocket } from "./gateway_socket";
import { NodeModel, type NodeView, hexNodeId, logModuleName, nodeColor } from "./nodes";

/** A node not heard for this long is stale, whatever the gateway still lists. */
const STALE_MS = 3000;

/** How long a toast stays on screen [ms]. */
const TOAST_MS = 6000;

export class Shell {
    /** Where the page draws itself. */
    readonly content: HTMLElement;
    /** Right-hand side of the toolbar, for the per-page controls. */
    readonly toolbar: HTMLElement;
    /** The node table every page reads its world from. */
    readonly nodes = new NodeModel();
    private readonly dot: HTMLElement;
    private readonly dotLabel: HTMLElement;
    private readonly chips: HTMLElement;
    private readonly counters: HTMLElement;
    private readonly toasts: HTMLElement;

    constructor(private readonly socket: GatewaySocket) {
        const nav = document.createElement("nav");
        nav.className = "nav";

        this.chips = document.createElement("div");
        this.chips.className = "discovery";
        nav.appendChild(this.chips);

        this.toolbar = document.createElement("div");
        this.toolbar.className = "nav-tools";
        nav.appendChild(this.toolbar);

        this.counters = document.createElement("span");
        this.counters.className = "nav-counters";
        nav.appendChild(this.counters);

        this.dot = document.createElement("span");
        this.dot.className = "dot";
        this.dotLabel = document.createElement("span");
        this.dotLabel.className = "nav-state";
        nav.appendChild(this.dot);
        nav.appendChild(this.dotLabel);

        this.content = document.createElement("main");
        this.content.className = "content";

        this.toasts = document.createElement("div");
        this.toasts.className = "toasts";

        document.body.appendChild(nav);
        document.body.appendChild(this.content);
        document.body.appendChild(this.toasts);
        this.paintChips([]);
        if (window.parent !== window) {
            forwardShortcuts();
        }

        this.nodes.onChange((nodes) => this.paintChips(nodes));
        socket.onState((state) => {
            this.dot.className = `dot ${state}`;
            this.dotLabel.textContent = state;
            if (state !== "open") {
                this.nodes.clear();
            }
        });
        socket.on("nodes", (table) => this.nodes.applyTable(table));
        socket.on("status", (status) => {
            this.nodes.setGatewayWireHash(status.wireHash);
            const parts: string[] = [];
            if (status.badFrames > 0) {
                parts.push(`${status.badFrames} bad`);
            }
            if (status.dropped > 0) {
                parts.push(`${status.dropped} dropped`);
            }
            if (status.rcClients > 1) {
                parts.push(`${status.rcClients} RC PILOTS`);
            }
            this.counters.textContent = parts.join(" | ");
        });
        socket.onEnvelope((src, envelope) => {
            this.nodes.noteFrame(src);
            // A log line of any node, the gateway included: only what needs
            // an operator is toasted, prefixed with the module that said it.
            if (envelope.body.case === "log" && envelope.body.value.level >= LogLevel.WARN) {
                const line = envelope.body.value;
                const node = this.nodes.get(src);
                const who = node?.name ?? `node ${hexNodeId(src)}`;
                this.notify(`${who} ${logModuleName(node, line.moduleId)}: ${line.text}`, false);
            }
        });
    }

    /** One line to the operator, shared by the page and the shell itself. */
    notify(text: string, ok: boolean): void {
        const toast = document.createElement("div");
        toast.className = ok ? "toast" : "toast bad";
        toast.textContent = text;
        this.toasts.appendChild(toast);
        setTimeout(() => toast.remove(), TOAST_MS);
    }

    /** Sends one request and reports its ack as a toast. */
    ask(message: GatewayMessage, what: string): void {
        void this.socket
            .request(message)
            .then((ack: Ack) => this.notify(ack.ok ? `${what}: ok` : `${what}: ${ack.error}`, ack.ok))
            .catch((error: unknown) => this.notify(`${what}: ${String(error)}`, false));
    }

    private paintChips(nodes: NodeView[]): void {
        this.chips.replaceChildren();
        if (nodes.length === 0) {
            const empty = document.createElement("span");
            empty.className = "discovery-empty";
            empty.textContent = "no node heard";
            this.chips.appendChild(empty);
            return;
        }
        for (const node of [...nodes].sort((a, b) => a.id - b.id)) {
            const chip = document.createElement("span");
            chip.className =
                "chip" +
                (node.ageMs > STALE_MS ? " stale" : "") +
                (node.wireMismatch ? " mismatch" : "");
            chip.style.borderLeft = `3px solid ${nodeColor(node.id)}`;
            if (node.wireMismatch) {
                chip.title = "built on another wire schema than the gateway: rebuild and reflash";
            }
            const name = document.createElement("b");
            name.textContent = node.kindName === node.name ? node.name : `${node.kindName} ${node.name}`;
            chip.appendChild(name);
            const detail = document.createElement("span");
            detail.textContent =
                ` node ${hexNodeId(node.id)}` +
                ` ${(node.ageMs / 1000).toFixed(1)} s ago` +
                (node.wireMismatch ? " WIRE MISMATCH" : "");
            chip.appendChild(detail);
            this.chips.appendChild(chip);
        }
    }
}

/**
 * Forwards shortcut-like keydowns (a modifier or an F key) to the embedding
 * page: key events never leave an iframe on their own, so editor bindings
 * like toggling the terminal go dead when the page has the focus. The
 * embedder re-dispatches them where the editor listens.
 */
function forwardShortcuts(): void {
    window.addEventListener("keydown", (event) => {
        if (!event.ctrlKey && !event.altKey && !event.metaKey && !/^F\d{1,2}$/.test(event.key)) {
            return;
        }
        window.parent.postMessage(
            {
                type: "mark4-shortcut",
                key: event.key,
                code: event.code,
                keyCode: event.keyCode,
                ctrlKey: event.ctrlKey,
                shiftKey: event.shiftKey,
                altKey: event.altKey,
                metaKey: event.metaKey,
                repeat: event.repeat,
            },
            "*",
        );
    });
}
