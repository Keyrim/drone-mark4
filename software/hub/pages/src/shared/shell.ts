/**
 * Page shell shared by the two hub windows: a thin top bar with the page's
 * own controls, the gateway connection state, and the toast strip every
 * page reports through. The inventory of nodes is not shown here: a page
 * shows drones, the editor extension shows the network.
 *
 * There is no in-page navigation: control and plots are two windows meant
 * to live on two screens, each with its own websocket to the gateway.
 */

import { type GatewayMessage } from "../gen/gateway_pb";
import { LogLevel } from "../gen/mark4_pb";
import { type Ack, type GatewaySocket } from "./gateway_socket";
import { NodeModel, hexNodeId, logModuleName } from "./nodes";

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
    private readonly pilots: HTMLElement;
    private readonly toasts: HTMLElement;

    constructor(private readonly socket: GatewaySocket) {
        const nav = document.createElement("nav");
        nav.className = "nav";

        this.toolbar = document.createElement("div");
        this.toolbar.className = "nav-tools";
        nav.appendChild(this.toolbar);

        // The one counter worth an operator's attention: two tabs piloting
        // the same drone is a safety matter, not an inventory line.
        this.pilots = document.createElement("span");
        this.pilots.className = "nav-pilots";
        nav.appendChild(this.pilots);

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
        if (window.parent !== window) {
            forwardShortcuts();
        }

        socket.onState((state) => {
            this.dot.className = `dot ${state}`;
            this.dotLabel.textContent = state === "open" ? "connected" : "reconnecting";
            if (state !== "open") {
                this.nodes.clear();
            }
        });
        socket.on("nodes", (table) => this.nodes.applyTable(table));
        socket.on("status", (status) => {
            this.nodes.setGatewayWireHash(status.wireHash);
            this.pilots.textContent = status.rcClients > 1 ? `${status.rcClients} RC PILOTS` : "";
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
