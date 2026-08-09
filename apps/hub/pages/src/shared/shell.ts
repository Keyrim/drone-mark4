/**
 * Page shell shared by every hub page: the top navigation, the connection
 * dot and the discovery bar naming the flight processes the hub can see.
 * A page builds a Shell, then fills shell.content with whatever it draws.
 */

import type { HubMessage, HubSocket } from "./hub_socket";

export type PageId = "plots" | "console" | "attitude";

const PAGES: { id: PageId; label: string; href: string }[] = [
    { id: "plots", label: "Plots", href: "index.html" },
    { id: "console", label: "Console", href: "console.html" },
    { id: "attitude", label: "Attitude", href: "attitude.html" },
];

interface DiscoveredProcess {
    kindName: string;
    sessionId: number;
    telemetryPort: number;
    viaSerial: boolean;
    ageMs: number;
}

/** A process not seen for this long is stale, whatever the hub still lists. */
const STALE_MS = 3000;

export class Shell {
    /** Where the page draws itself. */
    readonly content: HTMLElement;
    /** Right-hand side of the toolbar, for the per-page controls. */
    readonly toolbar: HTMLElement;
    private readonly dot: HTMLElement;
    private readonly dotLabel: HTMLElement;
    private readonly discovery: HTMLElement;
    private readonly counters: HTMLElement;

    constructor(socket: HubSocket, active: PageId) {
        const nav = document.createElement("nav");
        nav.className = "nav";

        const brand = document.createElement("span");
        brand.className = "nav-brand";
        brand.textContent = "mark4";
        nav.appendChild(brand);

        for (const page of PAGES) {
            const link = document.createElement("a");
            link.className = "nav-link" + (page.id === active ? " active" : "");
            link.href = page.href;
            link.textContent = page.label;
            nav.appendChild(link);
        }

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

        this.discovery = document.createElement("div");
        this.discovery.className = "discovery";

        this.content = document.createElement("main");
        this.content.className = "content";

        document.body.appendChild(nav);
        document.body.appendChild(this.discovery);
        document.body.appendChild(this.content);

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
        if (message["recording"] === true) {
            parts.push("recording");
        }
        this.counters.textContent = parts.join(" | ");
    }
}
