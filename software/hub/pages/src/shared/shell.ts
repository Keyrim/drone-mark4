/**
 * Page shell shared by the two hub windows: the top bar opens with the
 * chips naming the drones the hub can see (the page name already sits in
 * the tab title), then the connection dot, and the toast strip every page
 * reports through.
 *
 * There is no in-page navigation: control and plots are two windows meant
 * to live on two screens, each with its own websocket to the hub.
 */

import type { Ack, HubMessage, HubSocket } from "./hub_socket";

interface DiscoveredProcess {
    kindName: string;
    sessionId: number;
    ageMs: number;
    /** The node was built on another mark4.proto than the hub: it is listed, and mute. */
    wireMismatch: boolean;
}

/** A process not seen for this long is stale, whatever the hub still lists. */
const STALE_MS = 3000;

/** How long a toast stays on screen [ms]. */
const TOAST_MS = 6000;

export class Shell {
    /** Where the page draws itself. */
    readonly content: HTMLElement;
    /** Right-hand side of the toolbar, for the per-page controls. */
    readonly toolbar: HTMLElement;
    private readonly dot: HTMLElement;
    private readonly dotLabel: HTMLElement;
    private readonly discovery: HTMLElement;
    private readonly counters: HTMLElement;
    private readonly toasts: HTMLElement;

    constructor(private readonly socket: HubSocket) {
        const nav = document.createElement("nav");
        nav.className = "nav";

        this.discovery = document.createElement("div");
        this.discovery.className = "discovery";
        nav.appendChild(this.discovery);

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
        this.setDiscovery([]);
        if (window.parent !== window) {
            forwardShortcuts();
        }

        socket.onState((state) => {
            this.dot.className = `dot ${state}`;
            this.dotLabel.textContent = state;
            if (state !== "open") {
                this.setDiscovery([]);
            }
        });
        socket.on("discovery", (message: HubMessage) => {
            this.setDiscovery((message["processes"] as DiscoveredProcess[]) ?? []);
        });
        socket.on("status", (message: HubMessage) => this.setStatus(message));
        // A console line of a node that has no console on this desk: the
        // board behind its relay. Level 0 is INFO; anything above is bad.
        socket.on("log", (message: HubMessage) => {
            const level = typeof message["level"] === "number" ? (message["level"] as number) : 0;
            this.notify(`${String(message["source"])}: ${String(message["text"])}`, level === 0);
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
    ask(payload: Record<string, unknown>, what: string): void {
        void this.socket
            .request(payload)
            .then((ack: Ack) => this.notify(ack.ok ? `${what}: ok` : `${what}: ${ack.error}`, ack.ok))
            .catch((error: unknown) => this.notify(`${what}: ${String(error)}`, false));
    }

    private setDiscovery(processes: DiscoveredProcess[]): void {
        this.discovery.replaceChildren();
        if (processes.length === 0) {
            const empty = document.createElement("span");
            empty.className = "discovery-empty";
            empty.textContent = "no flight process announced";
            this.discovery.appendChild(empty);
            return;
        }
        for (const process of processes) {
            const chip = document.createElement("span");
            chip.className =
                "chip" +
                (process.ageMs > STALE_MS ? " stale" : "") +
                (process.wireMismatch ? " mismatch" : "");
            if (process.wireMismatch) {
                chip.title = "built on another wire schema than the hub: rebuild and reflash";
            }
            const name = document.createElement("b");
            name.textContent = process.kindName;
            chip.appendChild(name);
            const detail = document.createElement("span");
            detail.textContent =
                ` node ${process.sessionId}` +
                ` ${(process.ageMs / 1000).toFixed(1)} s ago` +
                (process.wireMismatch ? " WIRE MISMATCH" : "");
            chip.appendChild(detail);
            this.discovery.appendChild(chip);
        }
    }

    private setStatus(message: HubMessage): void {
        const counts = (message["counts"] ?? {}) as Record<string, number>;
        const parts: string[] = [];
        if ((counts["badFrames"] ?? 0) > 0) {
            parts.push(`${counts["badFrames"]} bad`);
        }
        const rcClients = Number(message["rcClients"] ?? 0);
        if (rcClients > 1) {
            parts.push(`${rcClients} RC PILOTS`);
        }
        this.counters.textContent = parts.join(" | ");
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
