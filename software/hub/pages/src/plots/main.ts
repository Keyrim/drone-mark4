/**
 * Telemetry page: one drone's measures, configured, then recorded and viewed.
 *
 * There is no catalog of series here. The source node publishes its own
 * table of measures (the gateway pulls it, `NodeTelemetry`), the page offers
 * exactly that, and enables the subset it is asked to record. The page has
 * two modes, each taking the whole window:
 *
 *   setup   pick a config or make one: tick measures, group them into
 *           lanes, set the period; nothing is streamed
 *   live    the lanes of that config; Record streams into them, Stop leaves
 *           what was recorded on screen, Export writes it as CSV
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
import {
    buildConfig,
    exportName,
    parseConfig,
    toCsv,
    type SeriesData,
    type ViewConfig,
} from "../telemetry/config";
import { ConfigPanel, DEFAULT_PERIOD_MS } from "../telemetry/config_panel";
import { type Descriptor, TelemetryModel, unitLabel } from "../telemetry/model";
import { loadLocal, saveLocal, store, WORKING_CONFIG_KEY, WORKING_NAME_KEY } from "../telemetry/store";

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

type Mode = "setup" | "live";

const socket = new GatewaySocket();
const shell = new Shell(socket);
const model = new TelemetryModel();

/** Every node's table, by node id, as the gateway published it. */
const tables = new Map<number, Descriptor[]>();

let mode: Mode = "setup";
let recording = false;
let sourceNode: number | null = null;
/** The named config the selection was loaded from or saved under, "" for none. */
let configName = "";
/**
 * A config waiting for a table to be applied against: the working config at
 * load, or a named one opened before the gateway published the node's
 * measures. Applied by the first bind that has a table.
 */
let pendingConfig: ViewConfig | null = null;
let effectivePeriodMs: number | null = null;
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
    scheduleAutosave();
    rebuildLanes();
    refreshToolbar();
});

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

// The toolbar has one set of controls per mode; the source selector and
// the feedback line are shared. Only the current mode's set is shown.
const setupTools = document.createElement("div");
setupTools.className = "tools";
const liveTools = document.createElement("div");
liveTools.className = "tools";

/** One line of feedback in the toolbar: the last thing that happened. */
const status = document.createElement("span");
status.className = "tool-note";

function toolbarButton(into: HTMLElement, label: string, onClick: () => void): HTMLButtonElement {
    const button = document.createElement("button");
    button.className = "btn";
    button.textContent = label;
    button.addEventListener("click", onClick);
    into.appendChild(button);
    return button;
}

function toolbarSelect(into: HTMLElement, title: string): HTMLSelectElement {
    const select = document.createElement("select");
    select.className = "config-select";
    select.title = title;
    into.appendChild(select);
    return select;
}

// Exactly one drone is drawn, by node id. The selector lists the drones of
// the table; switching it stops whatever was streaming and starts over.
const sourceSelect = toolbarSelect(shell.toolbar, "drone whose measures this page records");
sourceSelect.addEventListener("change", () => {
    const next = Number(sourceSelect.value);
    if (next === sourceNode) {
        return;
    }
    stopRecording();
    sourceNode = next;
    model.clearData();
    bindSource();
});

shell.toolbar.appendChild(setupTools);
shell.toolbar.appendChild(liveTools);
shell.toolbar.appendChild(status);

// Setup: the config this selection is, or becomes.
const configSelect = toolbarSelect(setupTools, "view configs stored on the hub");
configSelect.addEventListener("change", () => {
    if (configSelect.value !== "") {
        void loadNamedConfig(configSelect.value);
    }
});

toolbarButton(setupTools, "New", () => {
    stopRecording();
    pendingConfig = null;
    model.reset();
    configName = "";
    nameInput.value = "";
    saveLocal(WORKING_NAME_KEY, "");
    configPanel.setPeriod(DEFAULT_PERIOD_MS);
    bindSource();
    scheduleAutosave();
    say("");
});

// An inline field rather than prompt(): a modal dialog is dead inside an
// editor webview (sandboxed iframe), where prompt() returns null.
const nameInput = document.createElement("input");
nameInput.className = "config-title";
nameInput.placeholder = "config name";
setupTools.appendChild(nameInput);

const saveConfigButton = toolbarButton(setupTools, "Save", () => {
    void saveNamedConfig();
});

const deleteConfigButton = toolbarButton(setupTools, "Delete", () => {
    void deleteNamedConfig();
});

setupTools.appendChild(configPanel.periodControl);

const startButton = toolbarButton(setupTools, "Start", () => {
    if (model.list().length === 0) {
        say("tick at least one measure first");
        return;
    }
    setMode("live");
});

// Live: the recording and the view of it.
const liveLabel = document.createElement("span");
liveLabel.className = "live-label";
liveTools.appendChild(liveLabel);

const recordButton = toolbarButton(liveTools, "Record", () => {
    if (recording) {
        stopRecording();
        return;
    }
    if (sourceNode === null || model.enabledIds().length === 0) {
        say("no live drone exposes the selected measures");
        return;
    }
    model.clearData();
    startRecording();
});

const windowButtons = WINDOWS_S.map((seconds) =>
    toolbarButton(liveTools, `${seconds} s`, () => {
        windowS = seconds;
        follow = true;
        refreshToolbar();
        dirty = true;
    })
);

const followButton = toolbarButton(liveTools, "Follow", () => {
    follow = true;
    refreshToolbar();
    dirty = true;
});

const pauseButton = toolbarButton(liveTools, "Pause", () => {
    paused = !paused;
    refreshToolbar();
    dirty = true;
});

const exportButton = toolbarButton(liveTools, "Export CSV", () => {
    void exportCsv();
});

toolbarButton(liveTools, "Edit config", () => {
    stopRecording();
    setMode("setup");
});

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
    setupTools.hidden = mode !== "setup";
    liveTools.hidden = mode !== "live";
    configPanel.root.hidden = mode !== "setup";
    viewer.hidden = mode !== "live";

    windowButtons.forEach((button, i) => {
        button.classList.toggle("active", WINDOWS_S[i] === windowS);
    });
    followButton.classList.toggle("active", follow);
    pauseButton.classList.toggle("active", paused);
    pauseButton.textContent = paused ? "Resume" : "Pause";
    recordButton.textContent = recording ? "Stop" : "Record";
    recordButton.classList.toggle("active", recording);
    exportButton.disabled = model.durationS() === 0;
    startButton.disabled = model.list().length === 0;
    deleteConfigButton.disabled = configName === "";
    configSelect.value = configName;

    const name = configName === "" ? "unnamed config" : configName;
    const count = model.list().length;
    const period =
        effectivePeriodMs === null || effectivePeriodMs === configPanel.period()
            ? `${configPanel.period()} ms`
            : `${configPanel.period()} ms, node clamped to ${effectivePeriodMs} ms`;
    liveLabel.textContent = `${name}: ${count} series, ${period}`;
    document.title = `mark4 telemetry - ${name}`;
}

function setMode(next: Mode): void {
    mode = next;
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
        if (recording) {
            // The node vanished mid-recording: break the curves rather than
            // draw a chord across the hole, and keep what was recorded.
            model.markGap();
            stopRecording();
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
    if (pendingConfig !== null && table.length > 0) {
        applyConfig(pendingConfig);
        return;
    }
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
    if (recording) {
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

function startRecording(): void {
    recording = true;
    lastDataMs = Date.now();
    brokenBySilence = false;
    follow = true;
    sendEnable(configPanel.period());
    keepaliveTimer = setInterval(() => {
        sendEnable(configPanel.period());
        if (Date.now() - lastDataMs > SILENCE_MS && !brokenBySilence) {
            // The node stopped answering: an explicit hole, so the lanes
            // break instead of drawing a chord over the silence.
            brokenBySilence = true;
            model.markGap();
            say("the drone went silent: the curves are broken here");
            dirty = true;
        }
    }, KEEPALIVE_MS);
    refreshToolbar();
}

/** Stops the stream, when one runs. What was recorded stays on screen. */
function stopRecording(): void {
    if (!recording) {
        return;
    }
    recording = false;
    if (keepaliveTimer !== null) {
        clearInterval(keepaliveTimer);
        keepaliveTimer = null;
    }
    // One explicit stop, so the node does not keep streaming for the three
    // seconds its own keepalive would take to expire.
    sendEnable(0);
    effectivePeriodMs = null;
    configPanel.setEffectivePeriod(null);
    refreshToolbar();
}

socket.onEnvelope((src, envelope) => {
    if (src !== sourceNode) {
        return;
    }
    if (envelope.body.case === "telemetryAck") {
        effectivePeriodMs = envelope.body.value.periodMs;
        configPanel.setEffectivePeriod(effectivePeriodMs);
        refreshToolbar();
        return;
    }
    if (envelope.body.case !== "telemetryData") {
        return;
    }
    if (!recording || paused) {
        return;
    }
    lastDataMs = Date.now();
    brokenBySilence = false;
    model.ingest(
        Number(envelope.body.value.timestampUs),
        envelope.body.value.values.map((value) => ({ id: value.id, value: value.value }))
    );
    if (exportButton.disabled) {
        refreshToolbar();
    }
    dirty = true;
});

// A tab that goes away must not leave the node streaming: the node would
// only notice three seconds later, and a board has no bandwidth to waste.
for (const event of ["pagehide", "beforeunload"]) {
    addEventListener(event, () => {
        if (recording) {
            sendEnable(0);
        }
    });
}

/* -------------------- exports and configs -------------------- */

/** The samples of every selected series, in selection order. */
function seriesData(): SeriesData[] {
    return model.list().map((spec) => {
        const buffer = model.buffer(spec.name);
        return { t: buffer?.t ?? [], v: buffer?.v ?? [] };
    });
}

async function exportCsv(): Promise<void> {
    const name = exportName(configName, new Date());
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
    configSelect.value = configName;
}

async function saveNamedConfig(): Promise<void> {
    const name = nameInput.value.trim();
    if (name === "") {
        say("type a name for the config first");
        return;
    }
    // Saving over the config that was loaded is the expected edit; saving
    // over another one is asked twice.
    if (name !== configName) {
        const existing = await store.listConfigs();
        if ((existing.value ?? []).some((entry) => entry.name === name)) {
            if (!confirmed(saveConfigButton, `Overwrite ${name}?`)) {
                return;
            }
        }
    }
    const body = JSON.stringify(buildConfig(configPanel.period(), model.list()));
    const result = await store.writeConfig(name, body);
    if (!result.ok) {
        say(`the hub refused it: ${result.error ?? ""}`);
        return;
    }
    configName = name;
    saveLocal(WORKING_NAME_KEY, configName);
    say(`config ${name} saved`);
    await refreshConfigList();
    refreshToolbar();
}

async function loadNamedConfig(name: string): Promise<void> {
    const result = await store.readConfig(name);
    if (!result.ok || result.value === undefined) {
        configSelect.value = configName;
        say(`cannot read the config ${name}: ${result.error ?? ""}`);
        return;
    }
    const config = parseConfig(result.value);
    if (config === null) {
        configSelect.value = configName;
        say(`${name} is not a config of this page`);
        return;
    }
    stopRecording();
    model.clearData();
    configName = name;
    nameInput.value = name;
    saveLocal(WORKING_NAME_KEY, configName);
    applyConfig(config);
    scheduleAutosave();
    refreshToolbar();
    say(`config ${name} loaded`);
}

async function deleteNamedConfig(): Promise<void> {
    if (configName === "") {
        return;
    }
    if (!confirmed(deleteConfigButton, `Delete ${configName}?`)) {
        return;
    }
    const result = await store.deleteConfig(configName);
    say(result.ok ? `config ${configName} deleted` : `cannot delete it: ${result.error ?? ""}`);
    if (result.ok) {
        // The selection stays: what is gone is its name on the hub.
        configName = "";
        nameInput.value = "";
        saveLocal(WORKING_NAME_KEY, "");
    }
    await refreshConfigList();
    refreshToolbar();
}

/**
 * Applies a stored or auto-saved config to the selection. A config names
 * measures, not ids: it is applied against the source node's current table,
 * and a measure that node does not expose is dropped rather than kept as a
 * curve nothing can fill. With no table yet the config waits for one.
 */
function applyConfig(config: ViewConfig): void {
    configPanel.setPeriod(config.periodMs);
    const table = sourceNode === null ? [] : (tables.get(sourceNode) ?? []);
    if (table.length === 0) {
        pendingConfig = config;
        return;
    }
    pendingConfig = null;
    model.reset();
    for (const entry of config.series) {
        const descriptor = table.find((candidate) => candidate.name === entry.name);
        if (descriptor === undefined) {
            continue;
        }
        model.add(descriptor);
        model.setLane(entry.name, entry.laneId);
    }
    bindSource();
    configPanel.revealSelection();
}

/** Auto-saves what is ticked right now, debounced. */
function scheduleAutosave(): void {
    if (autosaveTimer !== null) {
        clearTimeout(autosaveTimer);
    }
    autosaveTimer = setTimeout(() => {
        autosaveTimer = null;
        saveLocal(WORKING_CONFIG_KEY, JSON.stringify(buildConfig(configPanel.period(), model.list())));
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
    if (!dirty || mode !== "live") {
        return;
    }
    dirty = false;

    if (follow && recording && !paused) {
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
// The working config is restored now and applied once a drone's table is
// there: the tables only arrive after the socket opens.
const working = parseConfig(loadLocal(WORKING_CONFIG_KEY) ?? "");
configName = loadLocal(WORKING_NAME_KEY) ?? "";
nameInput.value = configName;
if (working !== null) {
    applyConfig(working);
}
refreshToolbar();
void refreshConfigList();
requestAnimationFrame(frame);
