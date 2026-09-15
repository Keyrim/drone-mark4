/**
 * The tunable parameter table of one node, and the profiles the gateway
 * stores beside it.
 *
 * The gateway holds that table: it pulls it from the node page by page and
 * publishes it whole as NodeTuning, so this panel asks for a refresh and
 * paints what comes back. A write is a TuningCommand, and its answer is a
 * TuningResult the gateway publishes to every client, matched on (node,
 * parameter id): the value in effect afterwards, refused or not.
 */

import { create } from "@bufbuild/protobuf";

import { GatewayMessageSchema, type NodeTuning, ProfileCommand_Op, type TuningResult } from "../gen/gateway_pb";
import { type TuningAck, TuningStatus } from "../gen/mark4_pb";
import type { GatewaySocket } from "../shared/gateway_socket";

const STATUS_NAMES: Record<number, string> = {
    [TuningStatus.OK]: "ok",
    [TuningStatus.UNKNOWN_ID]: "unknownId",
    [TuningStatus.OUT_OF_BOUNDS]: "outOfBounds",
    [TuningStatus.LOCKED_WHILE_ARMED]: "lockedWhileArmed",
};

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

/** A TuningCommand for one node: the table again, or one parameter. */
function tuningCommand(
    node: number,
    action: { case: "refresh"; value: boolean } | { case: "set"; value: { id: number; value: number } } | { case: "get"; value: { id: number } }
) {
    return create(GatewayMessageSchema, { body: { case: "tuningCommand", value: { node, action } } });
}

/** A ProfileCommand message, the gateway-local service behind the profiles. */
function profileCommand(op: ProfileCommand_Op, fields: { name?: string; targetNode?: number; values?: { id: number; value: number }[] }) {
    return create(GatewayMessageSchema, {
        body: {
            case: "profileCommand",
            value: {
                op,
                name: fields.name ?? "",
                targetNode: fields.targetNode ?? 0,
                values: fields.values ?? [],
            },
        },
    });
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
    private readonly onTable = (published: NodeTuning): void => {
        if (published.node === this.nodeId) {
            this.onParams(published);
        }
    };
    private readonly onResult = (result: TuningResult): void => {
        if (result.node === this.nodeId && result.ack !== undefined) {
            this.onAck(result.ack);
        }
    };

    constructor(
        private readonly socket: GatewaySocket,
        private readonly nodeId: number,
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

        socket.on("nodeTuning", this.onTable);
        socket.on("tuningResult", this.onResult);
        socket.on("profiles", (list) => this.onProfiles(list.names));
        socket.on("profile", (profile) => {
            this.loaded = new Map(profile.values.map((pair) => [pair.id, pair.value]));
            this.loadedName = profile.name;
            this.render();
            this.notify(`profile ${this.loadedName} loaded, ${this.loaded.size} values`, true);
        });
    }

    /** Asks for the profile names; the table is read on demand. */
    start(): void {
        void this.socket.request(profileCommand(ProfileCommand_Op.LIST, {})).catch(() => undefined);
    }

    /** The widget is leaving: stop listening for the node's answers. */
    destroy(): void {
        this.socket.off("nodeTuning", this.onTable);
        this.socket.off("tuningResult", this.onResult);
    }

    /** Drops the table: it belongs to the process that answered it. */
    clear(): void {
        this.params.clear();
        this.rows.clear();
        this.body.replaceChildren();
        this.count = 0;
        this.note.textContent = "";
    }

    /** Asks the gateway to pull the node's table again. */
    refresh(): void {
        this.note.textContent = "reading the table...";
        this.ask(tuningCommand(this.nodeId, { case: "refresh", value: true }), "read table");
    }

    /** The node's whole table, as the gateway holds it. */
    private onParams(published: NodeTuning): void {
        this.params.clear();
        this.rows.clear();
        this.body.replaceChildren();
        published.infos.forEach((info, index) => {
            this.params.set(info.id, {
                index,
                name: info.name,
                value: info.value,
                minValue: info.minValue,
                maxValue: info.maxValue,
                armedChange: info.armedChange,
            });
        });
        this.count = this.params.size;
        this.render();
        this.note.textContent = `${this.count} parameters`;
    }

    private onAck(ack: TuningAck): void {
        const row = this.rows.get(ack.id);
        const param = this.params.get(ack.id);
        if (!row || !param) {
            return;
        }
        // The value in the ack is the one actually in effect, refused or not
        param.value = ack.value;
        row.input.value = String(param.value);
        const status = STATUS_NAMES[ack.status] ?? `status ${ack.status}`;
        row.status.textContent = status;
        row.status.className = ack.status === TuningStatus.OK ? "cell-ok" : "cell-bad";
        if (ack.status === TuningStatus.LOCKED_WHILE_ARMED) {
            this.notify(`${param.name} is locked while armed: disarm to change it`, false);
        } else if (ack.status !== TuningStatus.OK) {
            this.notify(`${param.name}: ${status}`, false);
        }
        this.render();
    }

    private onProfiles(names: string[]): void {
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

    private loadProfile(): void {
        const name = this.profileSelect.value;
        if (name === "") {
            this.notify("pick a profile first", false);
            return;
        }
        this.ask(profileCommand(ProfileCommand_Op.LOAD, { name }), `load ${name}`);
    }

    private pushProfile(): void {
        const name = this.profileSelect.value;
        if (name === "") {
            this.notify("pick a profile first", false);
            return;
        }
        this.ask(
            profileCommand(ProfileCommand_Op.PUSH, { name, targetNode: this.nodeId }),
            `push ${name} to node ${this.nodeId}`
        );
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
        const values = [...this.params].map(([id, param]) => ({ id, value: param.value }));
        this.ask(profileCommand(ProfileCommand_Op.SAVE, { name, values }), `save ${name}`);
    }

    private ask(message: ReturnType<typeof profileCommand> | ReturnType<typeof tuningCommand>, what: string): void {
        void this.socket
            .request(message)
            .then((ack) => this.notify(ack.ok ? `${what}: ok` : `${what}: ${ack.error}`, ack.ok))
            .catch((error: unknown) => this.notify(`${what}: ${String(error)}`, false));
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
            this.notify(`${param.name} must stay in [${param.minValue}, ${param.maxValue}]`, false);
            input.value = String(param.value);
            return;
        }
        void this.socket
            .request(tuningCommand(this.nodeId, { case: "set", value: { id: paramId, value } }))
            .then((ack) => {
                if (!ack.ok) {
                    this.notify(`${param.name}: ${ack.error}`, false);
                }
                // The value in effect comes back as a TuningResult, not here
            })
            .catch((error: unknown) => this.notify(`${param.name}: ${String(error)}`, false));
    }
}
