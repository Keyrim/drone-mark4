/**
 * Telemetry page: one drone's measures, configured, recorded, viewed, saved.
 *
 * There is no catalog of series here. The source node publishes its own
 * table of measures (the gateway pulls it, `NodeTelemetry`), the page offers
 * exactly that, and enables the subset it is asked to record. A session goes
 * through three phases:
 *
 *   configuring  tick measures, set the period, group them into lanes
 *   recording    the config is frozen; the node streams what was enabled
 *   viewing      no traffic at all; browse, save, export
 *
 * The enable is also the keepalive: the node stops streaming 3 s after the
 * last one, so a tab that crashes never leaves a board talking to nobody.
 */

import { create } from "@bufbuild/protobuf";
import type uPlot from "uplot";

import { EnvelopeSchema } from "../gen/mark4_pb";
import { decimateMinMax } from "../lanes/decimate";
import { LanesView } from "../lanes/lanes";
import { type LaneConfig } from "../lanes/model";
import { Ruler, RULER_H } from "../lanes/ruler";
import { clampToData, pan, ticks, zoom, type Viewport } from "../lanes/timebase";
import { frameMessage, GatewaySocket } from "../shared/gateway_socket";
import { nodeLabel } from "../shared/nodes";
import { Shell } from "../shared/shell";
import { clampPeriod, ConfigPanel } from "../telemetry/config_panel";
import { type Descriptor, TelemetryModel, unitLabel } from "../telemetry/model";
import {
    buildConfig,
    buildSession,
    parseConfig,
    parseSession,
    toCsv,
    type SeriesData,
} from "../telemetry/session";
import { loadWorkingConfig, saveWorkingConfig, store } from "../telemetry/store";

const WINDOWS_S = [5, 10, 20, 60];
const DEFAULT_WINDOW_S = 20;
const MAX_WINDOW_S = 3600;
const ZOOM_STEP = 1.2;

/** How often the enable is repeated while recording [ms]. */
const KEEPALIVE_MS = 1000;

/** Silence from the node after which the curves are broken [ms]. */
const SILENCE_MS = 3000;

/** How long the working config waits after an edit before being stored [ms]. */
const AUTOSAVE_MS = 500;

type Phase = "configuring" | "recording" | "viewing";

const socket = new GatewaySocket();
const shell = new Shell(socket);
const model = new TelemetryModel();

/** Every node's table, by node id, as the gateway published it. */
const tables = new Map<number, Descriptor[]>();

let phase: Phase = "configuring";
let sourceNode: number | null = null;
let sessionName = "";
let unsaved = false;
let lastDataMs = 0;
let brokenBySilence = false;

let windowS = DEFAULT_WINDOW_S;
let follow = true;
let paused = false;
let viewport: Viewport = { t0: 0, t1: windowS };
let cursorT: number | null = null;
let dirty = true;

let keepaliveTimer: ReturnType<typeof setInterval> | null = null;
let autosaveTimer: ReturnType<typeof setTimeout> | null = null;

/* -------------------- layout -------------------- */

const viewer = document.createElement("div");
viewer.className = "viewer";

const rulerCanvas = document.createElement("canvas");
rulerCanvas.className = "ruler";
viewer.appendChild(rulerCanvas);
const ruler = new Ruler(rulerCanvas);

const lanesScroll = document.createElement("div");
lanesScroll.className = "lanes-scroll";
viewer.appendChild(lanesScroll);

const lanesView = new LanesView(lanesScroll, {
    onCursorTime(t) {
        cursorT = t;
        dirty = true;
    },
    onNeedsRender() {
        dirty = true;
    },
});
lanesView.decimate = (t, v) => decimateMinMax(t, v) as unknown as uPlot.AlignedData;

const configPanel = new ConfigPanel(model, () => {
    unsaved = true;
    scheduleAutosave();
    rebuildLanes();
    refreshToolbar();
});

const status = document.createElement("div");
status.className = "config-note";

/** The session name and its unsaved mark, in the toolbar. */
const sessionLabel = document.createElement("span");
sessionLabel.className = "session-label";
shell.toolbar.appendChild(sessionLabel);

shell.content.appendChild(status);
shell.content.appendChild(configPanel.root);
shell.content.appendChild(viewer);

/** Rebuilds the lanes from the selection: one lane per lane id, in order. */
function rebuildLanes(): void {
    const lanes: LaneConfig[] = model.lanes().map((laneId) => {
        const series = model.list().filter((spec) => spec.laneId === laneId);
        const unit = series[0] === undefined ? "" : unitLabel(series[0].unit);
        return { title: unit === "" ? "unitless" : unit, keys: series.map((spec) => spec.name) };
    });
    const buffers = new Map(
        model
            .list()
            .map((spec) => [spec.name, model.buffer(spec.name)])
            .filter((entry): entry is [string, NonNullable<ReturnType<typeof model.buffer>>] => entry[1] !== undefined)
    );
    lanesView.setLanes(lanes, buffers);
    dirty = true;
}

/* -------------------- toolbar -------------------- */

function toolbarButton(label: string, onClick: () => void): HTMLButtonElement {
    const button = document.createElement("button");
    button.className = "btn";
    button.textContent = label;
    button.addEventListener("click", onClick);
    shell.toolbar.appendChild(button);
    return button;
}

// Exactly one drone is drawn, by node id. The selector lists the drones of
// the table; switching it stops whatever was streaming and starts over.
const sourceSelect = document.createElement("select");
sourceSelect.className = "config-select";
sourceSelect.title = "drone whose measures this page records";
sourceSelect.addEventListener("change", () => {
    const next = Number(sourceSelect.value);
    if (next === sourceNode) {
        return;
    }
    stopStream();
    sourceNode = next;
    model.clearData();
    bindSource();
    setPhase("configuring");
});
shell.toolbar.appendChild(sourceSelect);

const recordButton = toolbarButton("Record", () => {
    if (phase === "recording") {
        stopStream();
        setPhase("viewing");
        return;
    }
    if (sourceNode === null || model.enabledIds().length === 0) {
        say("tick at least one measure of a live drone first");
        return;
    }
    model.clearData();
    unsaved = true;
    setPhase("recording");
    startStream();
});

const newButton = toolbarButton("New session", () => {
    if (unsaved && !confirmed(newButton, "Discard?")) {
        return;
    }
    stopStream();
    sessionName = "";
    unsaved = false;
    model.clearData();
    setPhase("configuring");
    say("");
});

// An inline field rather than prompt(): a modal dialog is dead inside an
// editor webview (sandboxed iframe), where prompt() returns null.
const nameInput = document.createElement("input");
nameInput.className = "config-title";
nameInput.placeholder = "session name";
shell.toolbar.appendChild(nameInput);

const saveButton = toolbarButton("Save", () => {
    void saveSession();
});

const openSelect = document.createElement("select");
openSelect.className = "config-select";
openSelect.title = "sessions stored on the hub";
openSelect.addEventListener("change", () => {
    if (openSelect.value !== "") {
        void openSession(openSelect.value);
    }
});
shell.toolbar.appendChild(openSelect);

const exportButton = toolbarButton("Export CSV", () => {
    void exportCsv();
});

const configSelect = document.createElement("select");
configSelect.className = "config-select";
configSelect.title = "named view configs stored on the hub";
configSelect.addEventListener("change", () => {
    if (configSelect.value !== "") {
        void loadNamedConfig(configSelect.value);
    }
});
shell.toolbar.appendChild(configSelect);

const saveConfigButton = toolbarButton("Save config", () => {
    void saveNamedConfig();
});

const deleteConfigButton = toolbarButton("Delete config", () => {
    void deleteNamedConfig();
});

const windowButtons = WINDOWS_S.map((seconds) =>
    toolbarButton(`${seconds} s`, () => {
        windowS = seconds;
        follow = true;
        refreshToolbar();
        dirty = true;
    })
);

const followButton = toolbarButton("Follow", () => {
    follow = true;
    refreshToolbar();
    dirty = true;
});

const pauseButton = toolbarButton("Pause", () => {
    paused = !paused;
    refreshToolbar();
    dirty = true;
});

/** One line of feedback under the toolbar: the last thing that happened. */
function say(text: string): void {
    status.textContent = text;
}

/**
 * Two-click guard for the destructive buttons: the first click arms the
 * button and relabels it, the second one goes through. A confirm() dialog
 * is dead inside an editor webview, and this needs no dialog at all.
 */
const armed = new Map<HTMLButtonElement, ReturnType<typeof setTimeout>>();
function confirmed(button: HTMLButtonElement, label: string): boolean {
    const pending = armed.get(button);
    if (pending !== undefined) {
        clearTimeout(pending);
        armed.delete(button);
        button.textContent = button.dataset["label"] ?? button.textContent;
        button.classList.remove("danger");
        return true;
    }
    button.dataset["label"] = button.textContent ?? "";
    button.textContent = label;
    button.classList.add("danger");
    armed.set(
        button,
        setTimeout(() => {
            armed.delete(button);
            button.textContent = button.dataset["label"] ?? "";
            button.classList.remove("danger");
        }, 4000)
    );
    return false;
}

function refreshToolbar(): void {
    windowButtons.forEach((button, i) => {
        button.classList.toggle("active", WINDOWS_S[i] === windowS);
    });
    followButton.classList.toggle("active", follow);
    pauseButton.classList.toggle("active", paused);
    pauseButton.textContent = paused ? "Resume" : "Pause";
    recordButton.textContent = phase === "recording" ? "Stop" : "Record";
    recordButton.classList.toggle("active", phase === "recording");
    nameInput.value = nameInput.value === "" && sessionName !== "" ? sessionName : nameInput.value;
    const label = sessionName === "" ? "unnamed" : sessionName;
    sessionLabel.textContent = `${label}${unsaved ? " *" : ""} - ${phase}`;
    sessionLabel.classList.toggle("unsaved", unsaved);
    document.title = `mark4 telemetry - ${label}${unsaved ? " *" : ""}`;
    const idle = phase !== "recording";
    saveButton.disabled = model.list().length === 0;
    exportButton.disabled = model.list().length === 0;
    openSelect.disabled = !idle;
    configSelect.disabled = !idle;
    saveConfigButton.disabled = !idle;
    deleteConfigButton.disabled = !idle;
}

function setPhase(next: Phase): void {
    phase = next;
    configPanel.setFrozen(next === "recording");
    refreshToolbar();
    dirty = true;
}

/* -------------------- the source node -------------------- */

shell.nodes.onChange(() => {
    // Only a drone exposes measures, and the source follows the table: when
    // the recorded one leaves, the first remaining drone takes over.
    const drones = shell.nodes.drones();
    const next = drones.some((node) => node.id === sourceNode)
        ? sourceNode
        : (drones[0]?.id ?? null);
    if (next !== sourceNode) {
        if (phase === "recording") {
            // The node vanished mid-recording: break the curves rather than
            // draw a chord across the hole, and keep what was recorded.
            model.markGap();
            stopStream();
            setPhase("viewing");
            say("the drone left the table: recording stopped");
        }
        sourceNode = next;
        bindSource();
    }
    sourceSelect.replaceChildren();
    for (const node of drones) {
        const option = document.createElement("option");
        option.value = String(node.id);
        option.textContent = nodeLabel(node);
        sourceSelect.appendChild(option);
    }
    sourceSelect.value = sourceNode === null ? "" : String(sourceNode);
});

/** Rebinds the selection to the source node's current table, by name. */
function bindSource(): void {
    const table = sourceNode === null ? [] : (tables.get(sourceNode) ?? []);
    model.bind(table);
    configPanel.setDescriptors(table);
    configPanel.render();
    rebuildLanes();
    refreshToolbar();
}

socket.on("nodeTelemetry", (published) => {
    tables.set(
        published.node,
        published.descriptors.map((descriptor) => ({
            id: descriptor.id,
            name: descriptor.name,
            unit: descriptor.unit,
        }))
    );
    if (published.node !== sourceNode) {
        return;
    }
    bindSource();
    if (phase === "recording") {
        // The node published a table again: it rebooted, or the gateway
        // pulled it late. Either way the ids may have moved, so the enable
        // goes out again on the ids that are current.
        sendEnable(configPanel.period());
    }
});

/* -------------------- the stream -------------------- */

/**
 * Sends one TelemetryEnable to the source node. A period of 0 stops the
 * stream; anything else replaces the enabled set and rearms the keepalive
 * on the node's side.
 */
function sendEnable(periodMs: number): void {
    if (sourceNode === null) {
        return;
    }
    const envelope = create(EnvelopeSchema, {
        body: {
            case: "telemetryEnable",
            value: { ids: periodMs === 0 ? [] : model.enabledIds(), periodMs },
        },
    });
    socket.send(frameMessage(sourceNode, envelope));
}

function startStream(): void {
    lastDataMs = Date.now();
    brokenBySilence = false;
    sendEnable(configPanel.period());
    keepaliveTimer = setInterval(() => {
        sendEnable(configPanel.period());
        if (Date.now() - lastDataMs > SILENCE_MS && !brokenBySilence) {
            // The node stopped answering: an explicit hole, so the lanes
            // break instead of drawing a chord over the silence.
            brokenBySilence = true;
            model.markGap();
            configPanel.render();
            say("the drone went silent: the curves are broken here");
            dirty = true;
        }
    }, KEEPALIVE_MS);
}

function stopStream(): void {
    if (keepaliveTimer !== null) {
        clearInterval(keepaliveTimer);
        keepaliveTimer = null;
    }
    // One explicit stop, so the node does not keep streaming for the three
    // seconds its own keepalive would take to expire.
    sendEnable(0);
    configPanel.setEffectivePeriod(null);
}

socket.onEnvelope((src, envelope) => {
    if (src !== sourceNode) {
        return;
    }
    if (envelope.body.case === "telemetryAck") {
        configPanel.setEffectivePeriod(envelope.body.value.periodMs);
        return;
    }
    if (envelope.body.case !== "telemetryData") {
        return;
    }
    if (phase !== "recording" || paused) {
        return;
    }
    lastDataMs = Date.now();
    brokenBySilence = false;
    model.ingest(
        Number(envelope.body.value.timestampUs),
        envelope.body.value.values.map((value) => ({ id: value.id, value: value.value }))
    );
    unsaved = true;
    dirty = true;
});

// A tab that goes away must not leave the node streaming: the node would
// only notice three seconds later, and a board has no bandwidth to waste.
for (const event of ["pagehide", "beforeunload"]) {
    addEventListener(event, () => {
        if (phase === "recording") {
            sendEnable(0);
        }
    });
}

/* -------------------- sessions, exports, configs -------------------- */

/** The samples of every selected series, in selection order. */
function seriesData(): SeriesData[] {
    return model.list().map((spec) => {
        const buffer = model.buffer(spec.name);
        return { t: buffer?.t ?? [], v: buffer?.v ?? [] };
    });
}

function nodeDescription(): { id: number; kind: string; label: string } {
    const node = sourceNode === null ? undefined : shell.nodes.get(sourceNode);
    return {
        id: sourceNode ?? 0,
        kind: node?.kindName ?? "",
        label: node === undefined ? "" : nodeLabel(node),
    };
}

async function saveSession(): Promise<void> {
    const name = (nameInput.value.trim() !== "" ? nameInput.value : sessionName).trim();
    if (name === "") {
        say("type a name for the session first");
        return;
    }
    const session = buildSession(
        name,
        nodeDescription(),
        model.originUs() ?? 0,
        model.durationS(),
        configPanel.period(),
        model.list(),
        seriesData()
    );
    const result = await store.writeSession(name, JSON.stringify(session));
    if (!result.ok) {
        say(`the hub refused the session: ${result.error ?? "unknown reason"}`);
        return;
    }
    sessionName = name;
    unsaved = false;
    nameInput.value = "";
    say(`saved ${name} (${result.value ?? 0} bytes)`);
    refreshToolbar();
    await refreshSessionList();
}

async function openSession(name: string): Promise<void> {
    if (unsaved && !confirmed(newButton, "Discard?")) {
        openSelect.value = "";
        say("the current session is unsaved: New session twice to discard it");
        return;
    }
    const result = await store.readSession(name);
    openSelect.value = "";
    if (!result.ok || result.value === undefined) {
        say(`cannot read ${name}: ${result.error ?? "unknown reason"}`);
        return;
    }
    const session = parseSession(result.value);
    if (session === null) {
        say(`${name} is not a session of this page`);
        return;
    }
    stopStream();
    model.load(session.series, session.series);
    configPanel.setPeriod(session.periodMs);
    sessionName = session.name === "" ? name : session.name;
    unsaved = false;
    // A stored session is browsed, not streamed: nothing is bound to a node
    // until the config is used again for a recording.
    bindSource();
    setPhase("viewing");
    follow = false;
    say(`${sessionName}: ${session.series.length} series, ${session.durationS.toFixed(1)} s`);
}

async function exportCsv(): Promise<void> {
    const name = (nameInput.value.trim() !== "" ? nameInput.value : sessionName).trim();
    if (name === "") {
        say("type a name for the export first");
        return;
    }
    const csv = toCsv(model.list(), seriesData());
    const result = await store.writeExport(name, csv);
    if (!result.ok) {
        say(`the hub refused the export: ${result.error ?? "unknown reason"}`);
        return;
    }
    const path = store.exportPath(name);
    status.replaceChildren();
    status.append(`exported ${csv.length} bytes to `);
    const link = document.createElement("a");
    link.href = path;
    link.textContent = path;
    status.appendChild(link);
}

async function refreshSessionList(): Promise<void> {
    const result = await store.listSessions();
    openSelect.replaceChildren();
    const head = document.createElement("option");
    head.value = "";
    head.textContent = "open session...";
    openSelect.appendChild(head);
    for (const entry of result.value ?? []) {
        const option = document.createElement("option");
        option.value = entry.name;
        option.textContent = entry.name;
        openSelect.appendChild(option);
    }
}

async function refreshConfigList(): Promise<void> {
    const result = await store.listConfigs();
    configSelect.replaceChildren();
    const head = document.createElement("option");
    head.value = "";
    head.textContent = "load config...";
    configSelect.appendChild(head);
    for (const entry of result.value ?? []) {
        const option = document.createElement("option");
        option.value = entry.name;
        option.textContent = entry.name;
        configSelect.appendChild(option);
    }
}

async function saveNamedConfig(): Promise<void> {
    const name = nameInput.value.trim();
    if (name === "") {
        say("type a name for the config first");
        return;
    }
    const existing = await store.listConfigs();
    if ((existing.value ?? []).some((entry) => entry.name === name)) {
        if (!confirmed(saveConfigButton, `Overwrite ${name}?`)) {
            return;
        }
    }
    const body = JSON.stringify(buildConfig(configPanel.period(), model.list()));
    const result = await store.writeConfig(name, body);
    say(result.ok ? `config ${name} saved` : `the hub refused it: ${result.error ?? ""}`);
    await refreshConfigList();
    configSelect.value = "";
}

async function loadNamedConfig(name: string): Promise<void> {
    const result = await store.readConfig(name);
    configSelect.value = "";
    if (!result.ok || result.value === undefined) {
        say(`cannot read the config ${name}: ${result.error ?? ""}`);
        return;
    }
    applyConfigText(result.value, `config ${name} loaded`);
}

async function deleteNamedConfig(): Promise<void> {
    const name = nameInput.value.trim();
    if (name === "") {
        say("type the name of the config to delete");
        return;
    }
    if (!confirmed(deleteConfigButton, `Delete ${name}?`)) {
        return;
    }
    const result = await store.deleteConfig(name);
    say(result.ok ? `config ${name} deleted` : `cannot delete it: ${result.error ?? ""}`);
    await refreshConfigList();
}

/** Applies a stored or auto-saved config to the selection. */
function applyConfigText(text: string, message: string): void {
    const config = parseConfig(text);
    if (config === null) {
        return;
    }
    // A config names measures, not ids: it is applied against the source
    // node's current table, and a measure that node does not expose is
    // dropped rather than kept as a curve nothing can fill.
    const table = sourceNode === null ? [] : (tables.get(sourceNode) ?? []);
    model.reset();
    for (const entry of config.series) {
        const descriptor = table.find((candidate) => candidate.name === entry.name);
        if (descriptor === undefined) {
            continue;
        }
        model.add(descriptor);
        model.setLane(entry.name, entry.laneId);
    }
    configPanel.setPeriod(config.periodMs);
    bindSource();
    say(message);
}

/** Auto-saves what is ticked right now, debounced. */
function scheduleAutosave(): void {
    if (autosaveTimer !== null) {
        clearTimeout(autosaveTimer);
    }
    autosaveTimer = setTimeout(() => {
        autosaveTimer = null;
        saveWorkingConfig(JSON.stringify(buildConfig(configPanel.period(), model.list())));
    }, AUTOSAVE_MS);
}

/* -------------------- zoom and pan -------------------- */

function dataEndS(): number {
    return model.durationS();
}

function timeAt(clientX: number): number {
    const box = lanesView.box();
    const rect = lanesScroll.getBoundingClientRect();
    if (box === null || box.width <= 0) {
        return (viewport.t0 + viewport.t1) / 2;
    }
    const ratio = (clientX - rect.left - box.left) / box.width;
    return viewport.t0 + ratio * (viewport.t1 - viewport.t0);
}

lanesScroll.addEventListener(
    "wheel",
    (event: WheelEvent) => {
        if (!event.ctrlKey && !event.shiftKey && Math.abs(event.deltaY) < 1) {
            return;
        }
        event.preventDefault();
        follow = false;
        viewport = zoom(
            viewport,
            timeAt(event.clientX),
            event.deltaY < 0 ? ZOOM_STEP : 1 / ZOOM_STEP,
            MAX_WINDOW_S
        );
        windowS = viewport.t1 - viewport.t0;
        refreshToolbar();
        dirty = true;
    },
    { passive: false }
);

let dragX: number | null = null;
lanesScroll.addEventListener("pointerdown", (event: PointerEvent) => {
    if (event.button !== 0) {
        return;
    }
    dragX = event.clientX;
});
addEventListener("pointerup", () => {
    dragX = null;
});
addEventListener("pointermove", (event: PointerEvent) => {
    if (dragX === null) {
        return;
    }
    const box = lanesView.box();
    if (box === null || box.width <= 0) {
        return;
    }
    const deltaT = ((dragX - event.clientX) / box.width) * (viewport.t1 - viewport.t0);
    if (deltaT === 0) {
        return;
    }
    dragX = event.clientX;
    follow = false;
    viewport = pan(viewport, deltaT);
    refreshToolbar();
    dirty = true;
});

/* -------------------- render loop -------------------- */

function frame(): void {
    requestAnimationFrame(frame);
    if (!dirty) {
        return;
    }
    dirty = false;

    if (follow && phase === "recording" && !paused) {
        const end = Math.max(dataEndS(), windowS);
        viewport = { t0: end - windowS, t1: end };
    } else if (!follow) {
        viewport = clampToData(viewport, dataEndS());
    }

    const box = lanesView.box();
    const tickTimes = ticks(viewport, box?.width ?? lanesScroll.clientWidth);
    lanesView.render(viewport, tickTimes);
    if (box !== null) {
        ruler.render(viewport, tickTimes, box, cursorT);
    }
}

rulerCanvas.style.height = `${RULER_H}px`;
configPanel.setPeriod(clampPeriod(configPanel.period()));
const working = loadWorkingConfig();
if (working !== null) {
    applyConfigText(working, "");
}
refreshToolbar();
void refreshSessionList();
void refreshConfigList();
requestAnimationFrame(frame);
