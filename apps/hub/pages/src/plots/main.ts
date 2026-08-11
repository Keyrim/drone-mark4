/**
 * Plots page: the hub telemetry drawn as a stack of lanes.
 *
 * Two modes, and no third. In live mode the viewport rides the right edge of
 * the stream over a fixed window; zooming or panning leaves follow mode and
 * the Follow button comes back to it, and Pause freezes the whole pipe,
 * ingestion included. In replay mode the same lanes are fed from one
 * recording the hub holds. There is no session in between: the hub records,
 * and the blackbox is the session.
 */

import type uPlot from "uplot";

import { ConfigPanel } from "../lanes/configPanel";
import { decimateMinMax } from "../lanes/decimate";
import { LanesView } from "../lanes/lanes";
import { SeriesBuffer, type LaneConfig, type Mode } from "../lanes/model";
import { Ruler, RULER_H } from "../lanes/ruler";
import { clampToData, pan, ticks, zoom, type Viewport } from "../lanes/timebase";
import { HubSocket, type HubMessage } from "../shared/hub_socket";
import { DEFAULT_LANES, LIVE_SERIES, LiveSampler, SOURCE_NAMES } from "../shared/series";
import { Shell } from "../shared/shell";
import { ReplayPanel } from "./replay_panel";

const WINDOWS_S = [5, 10, 20, 60];
const DEFAULT_WINDOW_S = 20;
const US_PER_S = 1e6;

const socket = new HubSocket();
const shell = new Shell(socket);

const liveBuffers = new Map<string, SeriesBuffer>(
    LIVE_SERIES.map((def) => [def.key, new SeriesBuffer(def)])
);
let buffers = liveBuffers;
const sampler = new LiveSampler();

let mode: Mode = "live";
let originUs: number | null = null;
let windowS = DEFAULT_WINDOW_S;
let follow = true;
let paused = false;
let replayEndS = 0;
let viewport: Viewport = { t0: 0, t1: windowS };
let cursorT: number | null = null;
let dirty = true;

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

const configPanel = new ConfigPanel(DEFAULT_LANES, (lanes) => {
    if (mode === "live") {
        rebuildLanes(lanes);
    }
});
configPanel.root.hidden = true;

const replayPanel = new ReplayPanel(
    () => configPanel.currentLanes(),
    (filled, name) => {
        buffers = filled.buffers;
        replayEndS = filled.durationS;
        follow = false;
        viewport = { t0: 0, t1: Math.max(replayEndS, 1) };
        rebuildLanes(filled.lanes);
        document.title = `mark4 plots - ${name}`;
    }
);
replayPanel.root.hidden = true;

shell.content.appendChild(replayPanel.root);
shell.content.appendChild(viewer);
shell.content.appendChild(configPanel.root);

function rebuildLanes(lanes: LaneConfig[]): void {
    lanesView.setLanes(lanes, buffers);
    dirty = true;
}

/* -------------------- toolbar -------------------- */

function toolbarButton(label: string, onClick: (button: HTMLButtonElement) => void): HTMLButtonElement {
    const button = document.createElement("button");
    button.className = "btn";
    button.textContent = label;
    button.addEventListener("click", () => onClick(button));
    shell.toolbar.appendChild(button);
    return button;
}

// Several drones may stream at once; the lanes draw exactly one. The first
// source seen locks the selector, switching it starts the buffers over.
let sourceKind: number | null = null;
const seenSources = new Set<number>();
const sourceSelect = document.createElement("select");
sourceSelect.className = "config-select";
sourceSelect.title = "telemetry source the lanes draw";
sourceSelect.addEventListener("change", () => {
    sourceKind = Number(sourceSelect.value);
    for (const buffer of liveBuffers.values()) {
        buffer.clear();
    }
    originUs = null;
    dirty = true;
});
shell.toolbar.appendChild(sourceSelect);

function noteSource(kind: number): void {
    if (seenSources.has(kind)) {
        return;
    }
    seenSources.add(kind);
    if (sourceKind === null) {
        sourceKind = kind;
    }
    sourceSelect.replaceChildren();
    for (const id of [...seenSources].sort((a, b) => a - b)) {
        const option = document.createElement("option");
        option.value = String(id);
        option.textContent = SOURCE_NAMES.get(id) ?? `source ${id}`;
        sourceSelect.appendChild(option);
    }
    sourceSelect.value = String(sourceKind);
}

const modeButtons: HTMLButtonElement[] = (["live", "replay"] as Mode[]).map((wanted) =>
    toolbarButton(wanted === "live" ? "Live" : "Replay", () => void setMode(wanted))
);

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

const clearButton = toolbarButton("Clear", () => {
    for (const buffer of buffers.values()) {
        buffer.clear();
    }
    originUs = null;
    dirty = true;
});
clearButton.title = "Drop every buffered sample";

const configButton = toolbarButton("Lanes", () => {
    configPanel.root.hidden = !configPanel.root.hidden;
    refreshToolbar();
    dirty = true;
});

function refreshToolbar(): void {
    modeButtons[0]?.classList.toggle("active", mode === "live");
    modeButtons[1]?.classList.toggle("active", mode === "replay");
    const live = mode === "live";
    for (const button of [followButton, pauseButton, clearButton]) {
        button.hidden = !live;
    }
    windowButtons.forEach((button, i) => {
        button.hidden = !live;
        button.classList.toggle("active", WINDOWS_S[i] === windowS);
    });
    followButton.classList.toggle("active", follow);
    pauseButton.classList.toggle("active", paused);
    pauseButton.textContent = paused ? "Resume" : "Pause";
    configButton.classList.toggle("active", !configPanel.root.hidden);
}

/** Switch between the live stream and a recording, rebuilding the lanes. */
async function setMode(wanted: Mode): Promise<void> {
    if (wanted === mode) {
        return;
    }
    mode = wanted;
    replayPanel.root.hidden = wanted !== "replay";
    if (wanted === "live") {
        buffers = liveBuffers;
        follow = true;
        document.title = "mark4 plots";
        const url = new URL(location.href);
        url.searchParams.delete("rec");
        history.replaceState(null, "", url);
        rebuildLanes(configPanel.currentLanes());
    } else {
        lanesView.clear();
        await replayPanel.refresh();
    }
    refreshToolbar();
    dirty = true;
}

/* -------------------- ingestion -------------------- */

function relativeS(timestampUs: number): number {
    if (originUs === null) {
        originUs = timestampUs;
    }
    return (timestampUs - originUs) / US_PER_S;
}

socket.on("simRaw", (message: HubMessage) => {
    if (!paused && mode === "live") {
        // Latch only: the exact state is sampled by the next telemetry row
        sampler.latchSimRaw(message);
    }
});

socket.on("telemetry", (message: HubMessage) => {
    if (paused || mode !== "live") {
        return;
    }
    noteSource(Number(message["sourceId"]));
    if (Number(message["sourceId"]) !== sourceKind) {
        return;
    }
    const row = sampler.sample(message);
    const t = relativeS(row.timestampUs);
    for (const [key, value] of row.values) {
        buffers.get(key)?.push(t, value);
    }
    dirty = true;
});

/** Right edge of the data, in seconds. */
function dataEndS(): number {
    if (mode === "replay") {
        return replayEndS;
    }
    let end = 0;
    for (const buffer of buffers.values()) {
        end = Math.max(end, buffer.endS());
    }
    return end;
}

/* -------------------- zoom and pan -------------------- */

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
        viewport = zoom(viewport, timeAt(event.clientX), event.deltaY < 0 ? 1.2 : 1 / 1.2, 3600);
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

    if (follow && !paused) {
        const end = Math.max(dataEndS(), windowS);
        viewport = { t0: end - windowS, t1: end };
    } else if (!follow) {
        viewport = clampToData(viewport, dataEndS());
    }

    if (lanesView.isEmpty() && mode === "live") {
        rebuildLanes(configPanel.currentLanes());
    }
    const box = lanesView.box();
    const tickTimes = ticks(viewport, box?.width ?? lanesScroll.clientWidth);
    lanesView.render(viewport, tickTimes);
    if (box !== null) {
        ruler.render(viewport, tickTimes, box, cursorT);
    }
}

rulerCanvas.style.height = `${RULER_H}px`;
rebuildLanes(configPanel.currentLanes());
refreshToolbar();
requestAnimationFrame(frame);

// A ?rec= link opens straight into that recording
if (new URLSearchParams(location.search).has("rec")) {
    void setMode("replay");
}
