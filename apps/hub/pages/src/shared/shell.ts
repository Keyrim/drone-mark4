/**
 * Page shell shared by the two hub windows: the top bar with the connection
 * dot, the link that opens the other window, the discovery bar naming the
 * drones the hub can see, and the toast strip every page reports through.
 *
 * There is no in-page navigation: control and plots are two windows meant
 * to live on two screens, each with its own websocket to the hub.
 */

import type { Ack, HubMessage, HubSocket } from "./hub_socket";

export type PageId = "plots" | "console";

/** The window the top bar offers to open, from each page. */
const OTHER: Record<PageId, { label: string; href: string }> = {
    plots: { label: "Open control", href: "index.html" },
    console: { label: "Open plots", href: "plots.html" },
};

interface DiscoveredProcess {
    kindName: string;
    sessionId: number;
    telemetryPort: number;
    viaSerial: boolean;
    ageMs: number;
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

    constructor(private readonly socket: HubSocket, active: PageId) {
        const nav = document.createElement("nav");
        nav.className = "nav";

        const brand = document.createElement("span");
        brand.className = "nav-brand";
        brand.textContent = active === "plots" ? "mark4 plots" : "mark4 control";
        nav.appendChild(brand);

        this.toolbar = document.createElement("div");
        this.toolbar.className = "nav-tools";
        nav.appendChild(this.toolbar);

        this.counters = document.createElement("span");
        this.counters.className = "nav-counters";
        nav.appendChild(this.counters);

        const other = document.createElement("a");
        other.className = "btn nav-open";
        other.href = OTHER[active].href;
        other.target = "_blank";
        other.textContent = OTHER[active].label;
        nav.appendChild(other);

        this.dot = document.createElement("span");
        this.dot.className = "dot";
        this.dotLabel = document.createElement("span");
        this.dotLabel.className = "nav-state";
        nav.appendChild(this.dot);
        nav.appendChild(this.dotLabel);

        this.discovery = document.createElement("div");
        this.discovery.className = "discovery";

        this.content = document.createElement("main");
        this.content.className = "content";

        this.toasts = document.createElement("div");
        this.toasts.className = "toasts";

        document.body.appendChild(nav);
        document.body.appendChild(this.discovery);
        document.body.appendChild(this.content);
        document.body.appendChild(this.toasts);

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
            chip.className = "chip" + (process.ageMs > STALE_MS ? " stale" : "");
            const name = document.createElement("b");
            name.textContent = process.kindName;
            chip.appendChild(name);
            const detail = document.createElement("span");
            detail.textContent =
                ` session ${process.sessionId}` +
                ` ${process.viaSerial ? "serial" : `udp/${process.telemetryPort}`}` +
                ` ${(process.ageMs / 1000).toFixed(1)} s ago`;
            chip.appendChild(detail);
            this.discovery.appendChild(chip);
        }
    }

    private setStatus(message: HubMessage): void {
        const counts = (message["counts"] ?? {}) as Record<string, number>;
        const parts = [
            `${counts["telemetryRows"] ?? 0} tlm`,
            `${counts["simRawRows"] ?? 0} raw`,
            `${counts["blackboxRecords"] ?? 0} bb`,
        ];
        if ((counts["badFrames"] ?? 0) > 0) {
            parts.push(`${counts["badFrames"]} bad`);
        }
        parts.push(message["recording"] === true ? "capturing" : "not capturing");
        const rcClients = Number(message["rcClients"] ?? 0);
        if (rcClients > 1) {
            parts.push(`${rcClients} RC PILOTS`);
        }
        this.counters.textContent = parts.join(" | ");
    }
}
