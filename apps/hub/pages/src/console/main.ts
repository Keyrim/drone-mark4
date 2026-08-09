/**
 * Console page: the one place a human drives the system from.
 *
 * It picks a target among the processes the hub has discovered, shows what
 * that process reports, plays scenarios at it, tunes it, records it, and -
 * behind an explicit toggle - pilots it from the keyboard.
 *
 * Every command is a websocket message with a correlation id; the answer is
 * an ack that comes back to the toast strip. Nothing here reaches a socket or
 * a packed struct: the hub is the only translator in the system.
 */

import { HubSocket, type HubMessage } from "../shared/hub_socket";
import { Shell } from "../shared/shell";
import { FLIGHT_PHASE_NAMES } from "../shared/series";
import { listRecordings, type RecordingEntry } from "../shared/api";
import { Pilot } from "./pilot";
import { TuningPanel } from "./tuning";

/** How often the readout repaints: telemetry lands far faster than eyes read. */
const READOUT_MS = 100;

/** How long a toast stays on screen [ms]. */
const TOAST_MS = 6000;

/** How long a reboot stays armed waiting for its second click [ms]. */
const CONFIRM_MS = 4000;

const THROW_STATE_NAMES = ["idle", "thrust", "ballistic"];

const socket = new HubSocket();
const shell = new Shell(socket, "console");

let target = "";
let latest: HubMessage | null = null;

/* -------------------- toasts -------------------- */

const toasts = document.createElement("div");
toasts.className = "toasts";
document.body.appendChild(toasts);

function notify(text: string, ok: boolean): void {
    const toast = document.createElement("div");
    toast.className = ok ? "toast" : "toast bad";
    toast.textContent = text;
    toasts.appendChild(toast);
    setTimeout(() => toast.remove(), TOAST_MS);
}

/** Sends one request and reports its ack. */
function ask(payload: Record<string, unknown>, what: string): void {
    void socket
        .request(payload)
        .then((ack) => notify(ack.ok ? `${what}: ok` : `${what}: ${ack.error}`, ack.ok))
        .catch((error: unknown) => notify(`${what}: ${String(error)}`, false));
}

/* -------------------- small builders -------------------- */

function panel(title: string): { root: HTMLElement; bar: HTMLElement; body: HTMLElement } {
    const root = document.createElement("section");
    root.className = "panel";
    const bar = document.createElement("div");
    bar.className = "panel-bar";
    const head = document.createElement("b");
    head.textContent = title;
    bar.appendChild(head);
    const body = document.createElement("div");
    body.className = "panel-body";
    root.appendChild(bar);
    root.appendChild(body);
    return { root, bar, body };
}

function button(label: string, onClick: () => void, extra = ""): HTMLButtonElement {
    const element = document.createElement("button");
    element.className = `btn ${extra}`.trim();
    element.textContent = label;
    element.addEventListener("click", onClick);
    return element;
}

/** A labelled number input, and the reader that gives its current value. */
function field(label: string, value: number): { root: HTMLElement; read: () => number } {
    const root = document.createElement("label");
    root.className = "field";
    const name = document.createElement("span");
    name.textContent = label;
    const input = document.createElement("input");
    input.type = "number";
    input.step = "any";
    input.className = "config-title";
    input.value = String(value);
    root.appendChild(name);
    root.appendChild(input);
    return { root, read: () => Number(input.value) };
}

/* -------------------- target -------------------- */

const targetPanel = panel("Target");
const targetSelect = document.createElement("select");
targetSelect.className = "config-select";
targetSelect.addEventListener("change", () => {
    target = targetSelect.value;
    tuning.clear();
});
targetPanel.bar.appendChild(targetSelect);

socket.on("discovery", (message: HubMessage) => {
    const processes = (message["processes"] as { kindName: string }[]) ?? [];
    const names = [...new Set(processes.map((process) => process.kindName))];
    if (names.join() === [...targetSelect.options].map((option) => option.value).join()) {
        return;
    }
    targetSelect.replaceChildren();
    for (const name of names) {
        const option = document.createElement("option");
        option.value = name;
        option.textContent = name;
        targetSelect.appendChild(option);
    }
    // Keep the operator's choice when the process is still there
    if (names.includes(target)) {
        targetSelect.value = target;
    } else {
        target = names[0] ?? "";
        targetSelect.value = target;
        tuning.clear();
    }
});

/* -------------------- state readout -------------------- */

const readoutPanel = panel("State");
const readout = document.createElement("div");
readout.className = "readout";
readoutPanel.body.appendChild(readout);

const READINGS: [string, (message: HubMessage) => string][] = [
    ["phase", (m) => FLIGHT_PHASE_NAMES[Number(m["flightPhase"])] ?? String(m["flightPhase"])],
    ["throw", (m) => THROW_STATE_NAMES[Number(m["throwState"])] ?? String(m["throwState"])],
    ["throws", (m) => String(m["throwCount"])],
    ["altitude", (m) => `${Number(m["altitudeM"]).toFixed(2)} m`],
    ["vertical", (m) => `${Number(m["verticalVelocityMps"]).toFixed(2)} m/s`],
    [
        "motors",
        (m) =>
            ((m["motor"] as number[]) ?? []).map((value) => value.toFixed(2)).join("  ") || "-",
    ],
    ["apex", (m) => `${Number(m["apexAltitudeM"]).toFixed(2)} m`],
];

const readingValues = READINGS.map(([label]) => {
    const cell = document.createElement("div");
    cell.className = "reading";
    const name = document.createElement("span");
    name.textContent = label;
    const value = document.createElement("b");
    value.textContent = "-";
    cell.appendChild(name);
    cell.appendChild(value);
    readout.appendChild(cell);
    return value;
});

socket.on("telemetry", (message: HubMessage) => {
    latest = message;
});

setInterval(() => {
    if (latest === null) {
        return;
    }
    READINGS.forEach(([, read], index) => {
        readingValues[index]!.textContent = read(latest as HubMessage);
    });
    pilot.setTelemetry((latest["motor"] as number[]) ?? [], Number(latest["flightPhase"]));
}, READOUT_MS);

socket.on("status", (message: HubMessage) => {
    pilot.setClients(Number(message["clients"] ?? 1));
    recordButton.textContent = message["recording"] === true ? "Stop recording" : "Start recording";
    recordButton.classList.toggle("active", message["recording"] === true);
});

/* -------------------- session -------------------- */

const sessionPanel = panel("Session");
const recordButton = button("Start recording", () => {
    const action = recordButton.classList.contains("active") ? "stop" : "start";
    ask({ type: "record", action }, `record ${action}`);
});
sessionPanel.body.appendChild(recordButton);

// Two clicks, because a reboot in the middle of a bench run costs a run
let rebootArmedUntil = 0;
const rebootButton = button(
    "Reboot board",
    () => {
        if (Date.now() < rebootArmedUntil) {
            rebootArmedUntil = 0;
            rebootButton.textContent = "Reboot board";
            rebootButton.classList.remove("active");
            ask({ type: "reboot", target: "firmware" }, "reboot");
            return;
        }
        rebootArmedUntil = Date.now() + CONFIRM_MS;
        rebootButton.textContent = "Reboot board: click again";
        rebootButton.classList.add("active");
        setTimeout(() => {
            if (Date.now() >= rebootArmedUntil) {
                rebootButton.textContent = "Reboot board";
                rebootButton.classList.remove("active");
            }
        }, CONFIRM_MS);
    },
    "danger"
);
sessionPanel.body.appendChild(rebootButton);

/* -------------------- scenarios -------------------- */

const scenarioPanel = panel("Scenario");
const seed = field("seed", 1234);
const throwDelay = field("throwDelayUs", 2000000);
const velocityZ = field("velocityMps z", 6);
const spinZ = field("angularVelocityRadS z", 0);
const heldSeconds = field("heldSeconds", 1.5);
const heldTilt = field("heldTiltRad", 0.3);
const heldAzimuth = field("heldAzimuthRad", 0);
const swingSeconds = field("swingSeconds", 0.35);
for (const item of [
    seed,
    throwDelay,
    velocityZ,
    spinZ,
    heldSeconds,
    heldTilt,
    heldAzimuth,
    swingSeconds,
]) {
    scenarioPanel.body.appendChild(item.root);
}

function playScenario(scenario: string): void {
    const payload: Record<string, unknown> = { type: "simScenario", target, scenario };
    if (scenario !== "reset") {
        payload["velocityMps"] = [0, 0, velocityZ.read()];
        payload["angularVelocityRadS"] = [0, 0, spinZ.read()];
    }
    if (scenario === "throw") {
        payload["throwDelayUs"] = throwDelay.read();
    }
    if (scenario === "handThrow") {
        payload["heldSeconds"] = heldSeconds.read();
        payload["heldTiltRad"] = heldTilt.read();
        payload["heldAzimuthRad"] = heldAzimuth.read();
        payload["swingSeconds"] = swingSeconds.read();
    }
    payload["seed"] = seed.read();
    ask(payload, scenario);
}

for (const scenario of ["reset", "throw", "handThrow"]) {
    scenarioPanel.bar.appendChild(button(scenario, () => playScenario(scenario)));
}

/* -------------------- pilot -------------------- */

const pilotPanel = panel("Pilot");
const pilot = new Pilot(socket, () => target, notify);
pilotPanel.bar.appendChild(pilot.toggle);
pilotPanel.body.appendChild(pilot.controls());

/* -------------------- recordings -------------------- */

const recordingsPanel = panel("Recordings");
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

async function refreshRecordings(): Promise<void> {
    let entries: RecordingEntry[] = [];
    try {
        entries = (await listRecordings()).recordings;
    } catch (error) {
        notify(`recordings: ${String(error)}`, false);
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

recordingsPanel.bar.appendChild(recordingSelect);
recordingsPanel.bar.appendChild(speedSelect);
recordingsPanel.bar.appendChild(
    button("Re-execute", () => {
        if (recordingSelect.value === "") {
            notify("pick a recording first", false);
            return;
        }
        ask(
            { type: "replay", name: recordingSelect.value, speed: speedSelect.value },
            `replay ${recordingSelect.value}`
        );
    })
);
recordingsPanel.bar.appendChild(button("Refresh", () => void refreshRecordings()));
const recordingsNote = document.createElement("span");
recordingsNote.className = "panel-note";
recordingsNote.textContent =
    "the hub starts a drone_replay on it; it then announces itself and shows up as a target";
recordingsPanel.body.appendChild(recordingsNote);

/* -------------------- tuning -------------------- */

const tuning = new TuningPanel(socket, () => target, notify);

/* -------------------- layout -------------------- */

shell.content.className = "content console";
for (const section of [
    targetPanel.root,
    readoutPanel.root,
    pilotPanel.root,
    scenarioPanel.root,
    sessionPanel.root,
    recordingsPanel.root,
    tuning.root,
]) {
    shell.content.appendChild(section);
}

socket.onState((state) => {
    if (state === "open") {
        tuning.start();
    }
});
void refreshRecordings();
