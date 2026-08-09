/**
 * The tunable parameter table of one flight process, and the profiles the hub
 * stores beside it.
 *
 * The table is paged: `tuningList` only asks for a start index, and the
 * process unrolls one description per flight frame. The ack to the request
 * says it went out, nothing more, so the page watches the descriptions
 * arrive and resumes from the last index it saw when they stop coming - a
 * lost datagram costs one more request, never the whole table.
 *
 * A `tuningAck` is broadcast to every client and carries no correlation id,
 * so it is matched on (source, paramId): the answer to a write on this
 * process, for this parameter.
 */

import type { HubMessage, HubSocket } from "../shared/hub_socket";

/** No description for this long means the page asks again [ms]. */
const RESUME_AFTER_MS = 1500;

/** Requests a stalled table gets before the page gives up on it. */
const MAX_RESUMES = 5;

interface Param {
    index: number;
    name: string;
    value: number;
    minValue: number;
    maxValue: number;
    /** True when the flight core accepts a write while armed */
    armedChange: boolean;
}

interface Row {
    tr: HTMLTableRowElement;
    input: HTMLInputElement;
    status: HTMLElement;
}

export class TuningPanel {
    readonly root: HTMLElement;
    private readonly body: HTMLTableSectionElement;
    private readonly note: HTMLElement;
    private readonly profileSelect: HTMLSelectElement;
    private readonly params = new Map<number, Param>();
    private readonly rows = new Map<number, Row>();
    /** Values of the profile last loaded, for the diff against the table */
    private loaded: Map<number, number> | null = null;
    private loadedName = "";
    private count = 0;
    private highestIndex = -1;
    private resumes = 0;
    private watchdog: ReturnType<typeof setInterval> | null = null;

    constructor(
        private readonly socket: HubSocket,
        private readonly target: () => string,
        private readonly notify: (text: string, ok: boolean) => void
    ) {
        this.root = document.createElement("section");
        this.root.className = "panel";

        const bar = document.createElement("div");
        bar.className = "panel-bar";
        const title = document.createElement("b");
        title.textContent = "Tuning";
        bar.appendChild(title);

        const read = document.createElement("button");
        read.className = "btn";
        read.textContent = "Read table";
        read.addEventListener("click", () => this.refresh());
        bar.appendChild(read);

        this.profileSelect = document.createElement("select");
        this.profileSelect.className = "config-select";
        bar.appendChild(this.profileSelect);

        for (const [label, action] of [
            ["Load", () => this.loadProfile()],
            ["Push", () => this.pushProfile()],
            ["Save as...", () => this.saveProfile()],
        ] as [string, () => void][]) {
            const button = document.createElement("button");
            button.className = "btn";
            button.textContent = label;
            button.addEventListener("click", action);
            bar.appendChild(button);
        }

        this.note = document.createElement("span");
        this.note.className = "panel-note";
        bar.appendChild(this.note);

        const table = document.createElement("table");
        table.className = "table";
        const head = table.createTHead().insertRow();
        for (const label of ["name", "id", "value", "min", "max", "while armed", "profile", ""]) {
            const cell = document.createElement("th");
            cell.textContent = label;
            head.appendChild(cell);
        }
        this.body = table.createTBody();

        const scroll = document.createElement("div");
        scroll.className = "panel-scroll";
        scroll.appendChild(table);

        this.root.appendChild(bar);
        this.root.appendChild(scroll);

        socket.on("tuningInfo", (message) => this.onInfo(message));
        socket.on("tuningAck", (message) => this.onAck(message));
        socket.on("profiles", (message) => this.onProfiles(message));
        socket.on("profile", (message) => this.onProfile(message));
    }

    /** Asks for the profile names; the table is read on demand. */
    start(): void {
        void this.socket.request({ type: "profileList" }).catch(() => undefined);
    }

    /** Drops the table: it belongs to the process that answered it. */
    clear(): void {
        this.params.clear();
        this.rows.clear();
        this.body.replaceChildren();
        this.count = 0;
        this.highestIndex = -1;
        this.note.textContent = "";
        this.stopWatchdog();
    }

    /** Walks the table of the current target from the start. */
    refresh(): void {
        this.clear();
        this.resumes = 0;
        this.request(0);
        this.stopWatchdog();
        this.watchdog = setInterval(() => this.resume(), RESUME_AFTER_MS);
    }

    private request(startIndex: number): void {
        const target = this.target();
        if (target === "") {
            this.notify("no flight process to read the table from", false);
            return;
        }
        this.note.textContent = `reading from index ${startIndex}...`;
        void this.socket
            .request({ type: "tuningList", target, startIndex })
            .then((ack) => {
                if (!ack.ok) {
                    this.notify(`tuningList: ${ack.error}`, false);
                    this.stopWatchdog();
                }
            })
            .catch((error: unknown) => {
                this.notify(`tuningList: ${String(error)}`, false);
                this.stopWatchdog();
            });
    }

    /** Nothing arrived for a while: pick the walk back up where it stopped. */
    private resume(): void {
        if (this.count > 0 && this.params.size >= this.count) {
            this.note.textContent = `${this.count} parameters`;
            this.stopWatchdog();
            return;
        }
        if (this.resumes >= MAX_RESUMES) {
            this.note.textContent = `stalled at ${this.params.size}/${this.count || "?"}`;
            this.stopWatchdog();
            return;
        }
        this.resumes += 1;
        this.request(this.highestIndex + 1);
    }

    private onInfo(message: HubMessage): void {
        if (message["source"] !== this.target()) {
            return;
        }
        const paramId = Number(message["paramId"]);
        const index = Number(message["index"]);
        this.count = Number(message["count"]);
        this.highestIndex = Math.max(this.highestIndex, index);
        this.params.set(paramId, {
            index,
            name: String(message["name"] ?? ""),
            value: Number(message["value"]),
            minValue: Number(message["minValue"]),
            maxValue: Number(message["maxValue"]),
            armedChange: message["armedChange"] === true,
        });
        this.render();
        this.note.textContent = `${this.params.size}/${this.count} parameters`;
        if (this.params.size >= this.count) {
            this.stopWatchdog();
        }
    }

    private onAck(message: HubMessage): void {
        if (message["source"] !== this.target()) {
            return;
        }
        const paramId = Number(message["paramId"]);
        const row = this.rows.get(paramId);
        const param = this.params.get(paramId);
        if (!row || !param) {
            return;
        }
        // The value in the ack is the one actually in effect, refused or not
        param.value = Number(message["value"]);
        row.input.value = String(param.value);
        const status = String(message["statusName"] ?? "");
        row.status.textContent = status;
        row.status.className = status === "ok" ? "cell-ok" : "cell-bad";
        if (status === "lockedWhileArmed") {
            this.notify(
                `${param.name} is locked while armed: disarm to change it`,
                false
            );
        } else if (status !== "ok") {
            this.notify(`${param.name}: ${status}`, false);
        }
        this.render();
    }

    private onProfiles(message: HubMessage): void {
        const names = (message["names"] as string[]) ?? [];
        this.profileSelect.replaceChildren();
        const head = document.createElement("option");
        head.value = "";
        head.textContent = names.length === 0 ? "no profile stored" : "pick a profile...";
        this.profileSelect.appendChild(head);
        for (const name of names) {
            const option = document.createElement("option");
            option.value = name;
            option.textContent = name;
            this.profileSelect.appendChild(option);
        }
        if (this.loadedName !== "" && names.includes(this.loadedName)) {
            this.profileSelect.value = this.loadedName;
        }
    }

    private onProfile(message: HubMessage): void {
        const values = (message["values"] as Record<string, number>) ?? {};
        this.loaded = new Map(Object.entries(values).map(([id, value]) => [Number(id), value]));
        this.loadedName = String(message["name"] ?? "");
        this.render();
        this.notify(`profile ${this.loadedName} loaded, ${this.loaded.size} values`, true);
    }

    private loadProfile(): void {
        const name = this.profileSelect.value;
        if (name === "") {
            this.notify("pick a profile first", false);
            return;
        }
        this.ask({ type: "profileLoad", name }, `load ${name}`);
    }

    private pushProfile(): void {
        const name = this.profileSelect.value;
        const target = this.target();
        if (name === "" || target === "") {
            this.notify("pick a profile and a target first", false);
            return;
        }
        this.ask({ type: "profilePush", name, target }, `push ${name} to ${target}`);
    }

    private saveProfile(): void {
        if (this.params.size === 0) {
            this.notify("read the table first: there is nothing to save", false);
            return;
        }
        const name = prompt("Save the values on screen as which profile?", this.loadedName);
        if (name === null || name === "") {
            return;
        }
        const values: Record<string, number> = {};
        for (const [paramId, param] of this.params) {
            values[String(paramId)] = param.value;
        }
        this.ask({ type: "profileSave", name, values }, `save ${name}`);
    }

    private ask(payload: Record<string, unknown>, what: string): void {
        void this.socket
            .request(payload)
            .then((ack) => this.notify(ack.ok ? `${what}: ok` : `${what}: ${ack.error}`, ack.ok))
            .catch((error: unknown) => this.notify(`${what}: ${String(error)}`, false));
    }

    private stopWatchdog(): void {
        if (this.watchdog !== null) {
            clearInterval(this.watchdog);
            this.watchdog = null;
        }
    }

    private render(): void {
        const ordered = [...this.params.entries()].sort((a, b) => a[1].index - b[1].index);
        for (const [paramId, param] of ordered) {
            let row = this.rows.get(paramId);
            if (!row) {
                row = this.makeRow(paramId, param);
                this.rows.set(paramId, row);
                this.body.appendChild(row.tr);
            }
            const cells = row.tr.cells;
            cells[0]!.textContent = param.name;
            cells[1]!.textContent = String(paramId);
            if (document.activeElement !== row.input) {
                row.input.value = String(param.value);
            }
            cells[3]!.textContent = String(param.minValue);
            cells[4]!.textContent = String(param.maxValue);
            cells[5]!.textContent = param.armedChange ? "writable" : "locked";
            cells[5]!.className = param.armedChange ? "" : "cell-locked";
            const stored = this.loaded?.get(paramId);
            const differs = stored !== undefined && stored !== param.value;
            cells[6]!.textContent = stored === undefined ? "" : String(stored);
            cells[6]!.className = differs ? "cell-diff" : "";
        }
    }

    private makeRow(paramId: number, param: Param): Row {
        const tr = document.createElement("tr");
        for (let i = 0; i < 8; ++i) {
            tr.insertCell();
        }
        const input = document.createElement("input");
        input.type = "number";
        input.step = "any";
        input.className = "config-title";
        input.value = String(param.value);
        input.addEventListener("change", () => this.write(paramId, input));
        tr.cells[2]!.appendChild(input);
        const status = document.createElement("span");
        tr.cells[7]!.appendChild(status);
        return { tr, input, status };
    }

    private write(paramId: number, input: HTMLInputElement): void {
        const value = Number(input.value);
        const param = this.params.get(paramId);
        if (!param || !Number.isFinite(value)) {
            return;
        }
        if (value < param.minValue || value > param.maxValue) {
            this.notify(
                `${param.name} must stay in [${param.minValue}, ${param.maxValue}]`,
                false
            );
            input.value = String(param.value);
            return;
        }
        const target = this.target();
        void this.socket
            .request({ type: "tuningSet", target, paramId, value })
            .then((ack) => {
                if (!ack.ok) {
                    this.notify(`${param.name}: ${ack.error}`, false);
                }
                // The value in effect comes back as a tuningAck, not here
            })
            .catch((error: unknown) => this.notify(`${param.name}: ${String(error)}`, false));
    }
}
