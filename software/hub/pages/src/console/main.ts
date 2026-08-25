/**
 * Control page: one connected drone, its widget, and the 3D view.
 *
 * Whatever the route (announced UDP process, WiFi bridge, UART), the
 * workflow is the same: everything connectable is a row of the Connections
 * panel, and nothing is wired to the controls until the operator clicks
 * Connect. The hub holds the connection, so every tab shows the same drone.
 * A connected drone that goes silent keeps its widget and its row, marked
 * lost; the hub reconnects on its own when the same drone comes back, and
 * the transmitter is parked safe in between.
 *
 * Every command is a websocket message with a correlation id; the answer is
 * an ack that comes back to the toast strip. Nothing here reaches a socket
 * or a packed struct: the hub is the only translator in the system.
 */

import { listRecordings, type RecordingEntry } from "../shared/api";
import { HubSocket, type HubMessage } from "../shared/hub_socket";
import { Shell } from "../shared/shell";
import { AttitudePanel } from "./attitude_panel";
import {
    NO_CONNECTION,
    candidateRows,
    type AnnouncedBridge,
    type AnnouncedProcess,
    type Connection,
} from "./connection";
import { DroneWidget, type WidgetHooks } from "./drone_widget";
import { OtaPanel } from "./ota_panel";

const socket = new HubSocket();
const shell = new Shell(socket);

/** Recording last re-executed from this page, for the replay widget. */
let lastReplay: { name: string } | null = null;

const hooks: WidgetHooks = {
    notify: (text, ok) => shell.notify(text, ok),
    ask: (payload, what) => shell.ask(payload, what),
    lastReplay: () => lastReplay,
};

/* -------------------- the connected drone -------------------- */

const droneList = document.createElement("div");
droneList.className = "drone-list";
const empty = document.createElement("span");
empty.className = "panel-note";
empty.textContent = "no drone connected: pick one below";
droneList.appendChild(empty);

const attitude = new AttitudePanel(socket);

let connection: Connection = NO_CONNECTION;
let widget: DroneWidget | null = null;
/** Route and identity behind the current widget, "" when there is none. */
let widgetKey = "";

function applyConnection(next: Connection): void {
    const key = next.via === "none" ? "" : `${next.via}:${next.id}`;
    if (key !== widgetKey) {
        widget?.destroy();
        widget = null;
        widgetKey = key;
        if (key !== "") {
            widget = new DroneWidget(socket, next.kind, next.kindName, next.via !== "udp", hooks);
            droneList.appendChild(widget.root);
        }
    } else if (widget !== null && connection.live && !next.live) {
        // The drone was lost: park the transmitter safe, so the drone that
        // comes back (usually rebooted) is not greeted with an armed stick.
        widget.killNow();
    }
    connection = next;
    empty.hidden = widget !== null;
    attitude.setActive(new Set(widget === null ? [] : [connection.kind]));
    renderConnections();
}

socket.on("telemetry", (message: HubMessage) => {
    if (widget !== null && Number(message["sourceId"]) === connection.kind) {
        widget.onTelemetry(message);
    }
});

/* -------------------- connections -------------------- */

let processes: AnnouncedProcess[] = [];
let bridges: AnnouncedBridge[] = [];

const connectBlock = document.createElement("section");
connectBlock.className = "panel";
const connectBar = document.createElement("div");
connectBar.className = "panel-bar";
const connectTitle = document.createElement("b");
connectTitle.textContent = "Connections";
connectBar.appendChild(connectTitle);
const connectHint = document.createElement("span");
connectHint.className = "panel-note";
connectHint.textContent = "drones and bridges announce themselves here";
connectBar.appendChild(connectHint);
connectBlock.appendChild(connectBar);

const candidateList = document.createElement("div");
connectBlock.appendChild(candidateList);

function stateChip(state: "connected" | "lost"): HTMLElement {
    const chip = document.createElement("span");
    chip.className = `conn-state ${state}`;
    chip.textContent = state === "connected" ? "connected" : "connection lost, waiting";
    return chip;
}

function disconnectButton(): HTMLButtonElement {
    const button = document.createElement("button");
    button.className = "btn active";
    button.textContent = "Disconnect";
    button.addEventListener("click", () => shell.ask({ type: "disconnect" }, "disconnect"));
    return button;
}

function renderConnections(): void {
    candidateList.replaceChildren();
    const rows = candidateRows(processes, bridges, connection);
    if (rows.length === 0) {
        const note = document.createElement("div");
        note.className = "panel-body";
        const text = document.createElement("span");
        text.className = "panel-note";
        text.textContent = "nothing on the network: start a drone, power a bridge, or open the UART";
        note.appendChild(text);
        candidateList.appendChild(note);
    }
    for (const row of rows) {
        const line = document.createElement("div");
        line.className = "panel-body";
        const name = document.createElement("b");
        name.textContent = row.label;
        line.appendChild(name);
        const detail = document.createElement("span");
        detail.className = "panel-note";
        detail.textContent = row.detail;
        line.appendChild(detail);
        if (row.state !== "available") {
            line.appendChild(stateChip(row.state));
            line.appendChild(disconnectButton());
        } else if (row.connect !== null) {
            const payload = row.connect;
            const button = document.createElement("button");
            button.className = "btn";
            button.textContent = "Connect";
            button.addEventListener("click", () => shell.ask(payload, `connect ${row.label}`));
            line.appendChild(button);
        }
        candidateList.appendChild(line);
    }

    // The UART is the one door nothing announces, so it is always offered.
    const uartRow = document.createElement("div");
    uartRow.className = "panel-body";
    const uartConnected = connection.via === "uart";
    uartDevice.disabled = uartConnected;
    uartBaud.disabled = uartConnected;
    uartRow.appendChild(uartDevice);
    uartRow.appendChild(uartBaud);
    if (uartConnected) {
        uartRow.appendChild(stateChip(connection.live ? "connected" : "lost"));
        uartRow.appendChild(disconnectButton());
    } else {
        const button = document.createElement("button");
        button.className = "btn";
        button.textContent = "Connect";
        button.addEventListener("click", () => {
            shell.ask(
                {
                    type: "connect",
                    via: "uart",
                    device: uartDevice.value,
                    baud: Number(uartBaud.value),
                },
                `connect ${uartDevice.value}`,
            );
        });
        uartRow.appendChild(button);
    }
    candidateList.appendChild(uartRow);
}

const uartDevice = document.createElement("input");
uartDevice.className = "config-title";
uartDevice.value = "/dev/ttyUSB0";
uartDevice.title = "UART the board is wired to";
const uartBaud = document.createElement("input");
uartBaud.className = "config-title baud";
uartBaud.value = "921600";
uartBaud.title = "line speed [baud]";

socket.on("discovery", (message: HubMessage) => {
    processes = (message["processes"] as AnnouncedProcess[]) ?? [];
    bridges = (message["bridges"] as AnnouncedBridge[]) ?? [];
    renderConnections();
});

socket.on("status", (message: HubMessage) => {
    const raw = message["connection"] as Connection | undefined;
    const next = raw ?? NO_CONNECTION;
    if (
        next.via !== connection.via ||
        next.id !== connection.id ||
        next.live !== connection.live
    ) {
        applyConnection(next);
    }
});

/* -------------------- replay -------------------- */

// Starting a replay is not connecting to one: the drone_replay child
// announces itself like any other drone and shows up as a candidate above.
const replayBlock = document.createElement("section");
replayBlock.className = "panel";
const replayBar = document.createElement("div");
replayBar.className = "panel-bar";
const replayTitle = document.createElement("b");
replayTitle.textContent = "Blackbox replay";
replayBar.appendChild(replayTitle);
replayBlock.appendChild(replayBar);
const replayRow = document.createElement("div");
replayRow.className = "panel-body";

const recordingSelect = document.createElement("select");
recordingSelect.className = "config-select";
const speedSelect = document.createElement("select");
speedSelect.className = "config-select";
for (const speed of ["1", "4", "max"]) {
    const option = document.createElement("option");
    option.value = speed;
    option.textContent = speed === "max" ? "as fast as possible" : `${speed}x`;
    speedSelect.appendChild(option);
}
const startReplay = document.createElement("button");
startReplay.className = "btn";
startReplay.textContent = "Re-execute";
startReplay.title = "start a drone_replay on this blackbox: it announces itself as a drone";
startReplay.addEventListener("click", () => {
    if (recordingSelect.value === "") {
        shell.notify("pick a blackbox recording first", false);
        return;
    }
    lastReplay = { name: recordingSelect.value };
    shell.ask(
        { type: "replay", name: recordingSelect.value, speed: speedSelect.value },
        `replay ${recordingSelect.value}`
    );
});
const refresh = document.createElement("button");
refresh.className = "btn";
refresh.textContent = "Refresh";
refresh.addEventListener("click", () => void refreshRecordings());
replayRow.appendChild(recordingSelect);
replayRow.appendChild(speedSelect);
replayRow.appendChild(startReplay);
replayRow.appendChild(refresh);
replayBlock.appendChild(replayRow);

async function refreshRecordings(): Promise<void> {
    let entries: RecordingEntry[] = [];
    try {
        entries = (await listRecordings()).recordings;
    } catch (error) {
        shell.notify(`recordings: ${String(error)}`, false);
    }
    // Only a blackbox holds the sensor frames a replay steps through
    const playable = entries.filter((entry) => entry.kind === "blackbox");
    recordingSelect.replaceChildren();
    const head = document.createElement("option");
    head.value = "";
    head.textContent = playable.length === 0 ? "no blackbox recording" : "pick a recording...";
    recordingSelect.appendChild(head);
    for (const entry of playable) {
        const option = document.createElement("option");
        option.value = entry.name;
        option.textContent = `${entry.name} (${entry.estimatedRecords ?? 0} records)`;
        recordingSelect.appendChild(option);
    }
}

renderConnections();
void refreshRecordings();

/* -------------------- firmware update -------------------- */

// Reflashing the board is an operation on the drone that is already there,
// not a way to add one, so it sits below the drone list rather than inside a
// widget: it has to stay readable while the board is rebooting and its
// widget has momentarily disappeared.
const update = new OtaPanel(socket, (text, ok) => shell.notify(text, ok));

/* -------------------- layout -------------------- */

shell.content.className = "content console";
const left = document.createElement("div");
left.className = "console-col";
left.appendChild(droneList);
left.appendChild(update.root);
left.appendChild(connectBlock);
left.appendChild(replayBlock);
const right = document.createElement("div");
right.className = "console-col observe";
right.appendChild(attitude.root);
shell.content.appendChild(left);
shell.content.appendChild(right);
