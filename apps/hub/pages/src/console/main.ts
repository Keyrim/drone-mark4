/**
 * Control page: the connected drones, each in its own widget, all of them
 * superimposed in one 3D view.
 *
 * The left column mirrors what the hub sees: one widget per drone, whatever
 * its nature (real board, simulator, replay), created and removed by
 * discovery alone. UDP drones appear on their own; the "Add drone" block at
 * the bottom holds the two manual doors - opening the board UART, starting
 * a replay on a stored blackbox - which both end with the new drone
 * announcing itself and its widget appearing like any other.
 *
 * Every command is a websocket message with a correlation id; the answer is
 * an ack that comes back to the toast strip. Nothing here reaches a socket
 * or a packed struct: the hub is the only translator in the system.
 */

import { listRecordings, type RecordingEntry } from "../shared/api";
import { HubSocket, type HubMessage } from "../shared/hub_socket";
import { Shell } from "../shared/shell";
import { AttitudePanel } from "./attitude_panel";
import { DroneWidget, type WidgetHooks } from "./drone_widget";

const socket = new HubSocket();
const shell = new Shell(socket, "console");

/** Recording last re-executed from this page, for the replay widget. */
let lastReplay: { name: string } | null = null;

const hooks: WidgetHooks = {
    notify: (text, ok) => shell.notify(text, ok),
    ask: (payload, what) => shell.ask(payload, what),
    lastReplay: () => lastReplay,
};

/* -------------------- the drone list -------------------- */

const droneList = document.createElement("div");
droneList.className = "drone-list";
const empty = document.createElement("span");
empty.className = "panel-note";
empty.textContent = "no drone connected: start one, or add one below";
droneList.appendChild(empty);

const widgets = new Map<number, DroneWidget>();

interface DiscoveredProcess {
    kind: number;
    kindName: string;
    viaSerial: boolean;
}

const attitude = new AttitudePanel(socket);

socket.on("discovery", (message: HubMessage) => {
    const processes = (message["processes"] as DiscoveredProcess[]) ?? [];
    const alive = new Set(processes.map((process) => process.kind));
    for (const process of processes) {
        if (!widgets.has(process.kind)) {
            const widget = new DroneWidget(
                socket,
                process.kind,
                process.kindName,
                process.viaSerial,
                hooks
            );
            widgets.set(process.kind, widget);
            droneList.appendChild(widget.root);
        }
    }
    for (const [kind, widget] of widgets) {
        if (!alive.has(kind)) {
            widget.destroy();
            widgets.delete(kind);
        }
    }
    empty.hidden = widgets.size > 0;
    attitude.setActive(alive);
});

socket.on("telemetry", (message: HubMessage) => {
    widgets.get(Number(message["sourceId"]))?.onTelemetry(message);
});

/* -------------------- add drone -------------------- */

// UDP drones add themselves through discovery; these are the manual doors.
// The two ways in are exclusive and deliberately presented as a choice: the
// rows only appear once the operator has said which door they are opening.
const addBlock = document.createElement("section");
addBlock.className = "panel add-drone";
const addBar = document.createElement("div");
addBar.className = "panel-bar";
const addTitle = document.createElement("b");
addTitle.textContent = "Add drone";
addBar.appendChild(addTitle);
const uartTab = document.createElement("button");
uartTab.className = "btn add-tab";
uartTab.textContent = "Real board (UART)";
const replayTab = document.createElement("button");
replayTab.className = "btn add-tab";
replayTab.textContent = "Blackbox replay";
addBar.appendChild(uartTab);
addBar.appendChild(replayTab);
const addHint = document.createElement("span");
addHint.className = "panel-note";
addHint.textContent = "simulated drones appear on their own";
addBar.appendChild(addHint);
addBlock.appendChild(addBar);

const uartRow = document.createElement("div");
uartRow.className = "panel-body";
uartRow.hidden = true;
const uartDevice = document.createElement("input");
uartDevice.className = "config-title";
uartDevice.value = "/dev/ttyUSB0";
uartDevice.title = "UART the board is wired to";
const uartBaud = document.createElement("input");
uartBaud.className = "config-title baud";
uartBaud.value = "921600";
uartBaud.title = "line speed [baud]";
let serialOpen = false;
const uartButton = document.createElement("button");
uartButton.className = "btn";
uartButton.textContent = "Open UART";
uartButton.addEventListener("click", () => {
    const payload = serialOpen
        ? { type: "serial", action: "close" }
        : {
              type: "serial",
              action: "open",
              device: uartDevice.value,
              baud: Number(uartBaud.value),
          };
    shell.ask(payload, serialOpen ? "uart close" : `uart open ${uartDevice.value}`);
});
uartRow.appendChild(uartDevice);
uartRow.appendChild(uartBaud);
uartRow.appendChild(uartButton);
addBlock.appendChild(uartRow);

const replayRow = document.createElement("div");
replayRow.className = "panel-body";
replayRow.hidden = true;
uartTab.addEventListener("click", () => {
    uartRow.hidden = false;
    replayRow.hidden = true;
    uartTab.classList.add("active");
    replayTab.classList.remove("active");
});
replayTab.addEventListener("click", () => {
    replayRow.hidden = false;
    uartRow.hidden = true;
    replayTab.classList.add("active");
    uartTab.classList.remove("active");
    void refreshRecordings();
});
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
addBlock.appendChild(replayRow);

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

socket.on("status", (message: HubMessage) => {
    serialOpen = message["serialOpen"] === true;
    uartButton.textContent = serialOpen ? "Close UART" : "Open UART";
    uartButton.classList.toggle("active", serialOpen);
    uartDevice.disabled = serialOpen;
    uartBaud.disabled = serialOpen;
});

void refreshRecordings();

/* -------------------- layout -------------------- */

shell.content.className = "content console";
const left = document.createElement("div");
left.className = "console-col";
left.appendChild(droneList);
left.appendChild(addBlock);
const right = document.createElement("div");
right.className = "console-col observe";
right.appendChild(attitude.root);
shell.content.appendChild(left);
shell.content.appendChild(right);
