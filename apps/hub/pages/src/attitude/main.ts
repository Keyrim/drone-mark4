/**
 * Attitude page: the estimated attitude drawn as a drone gizmo, with the
 * exact simulator attitude behind it as a translucent ghost.
 *
 * The gizmo is fed by the telemetry stream of one source; several flight
 * processes may stream at once, so a selector says which one drives it. The
 * ghost comes from the simRaw stream, which only a simulator emits.
 */

import { HubSocket, type HubMessage } from "../shared/hub_socket";
import { asQuat, errorAngleDeg, eulerDeg, type Quat } from "../shared/quat";
import { Shell } from "../shared/shell";
import { isUsable, toRenderQuat } from "./remap";
import { AttitudeScene } from "./scene";

/** StreamSource of the protocol header, for the selector labels. */
const SOURCE_NAMES = new Map<number, string>([
    [1, "firmware"],
    [2, "drone_sim"],
    [3, "drone_replay"],
    [4, "sim_plant"],
]);

const socket = new HubSocket();
const shell = new Shell(socket, "attitude");

/* -------------------- controls -------------------- */

const sourceSelect = document.createElement("select");
sourceSelect.className = "config-select";
sourceSelect.title = "telemetry source driving the gizmo";
shell.toolbar.appendChild(sourceSelect);

const ghostButton = document.createElement("button");
ghostButton.className = "btn active";
ghostButton.textContent = "Ghost";
ghostButton.title = "exact simulator attitude";
shell.toolbar.appendChild(ghostButton);

/* -------------------- layout -------------------- */

const stage = document.createElement("div");
stage.className = "stage";
shell.content.appendChild(stage);

const canvasBox = document.createElement("div");
canvasBox.className = "canvas-box";
stage.appendChild(canvasBox);

const readouts = document.createElement("div");
readouts.className = "readouts";
stage.appendChild(readouts);

function addReadout(title: string): HTMLElement {
    const card = document.createElement("div");
    card.className = "card";
    const label = document.createElement("div");
    label.className = "card-title";
    label.textContent = title;
    const value = document.createElement("div");
    value.className = "readout-value";
    card.appendChild(label);
    card.appendChild(value);
    readouts.appendChild(card);
    return value;
}

const quatValue = addReadout("quaternion w x y z");
const eulerValue = addReadout("euler roll pitch yaw [deg]");
const errorValue = addReadout("attitude error [deg]");

const scene = new AttitudeScene(canvasBox);
new ResizeObserver(() => scene.resize()).observe(canvasBox);

/* -------------------- streams -------------------- */

/** Last usable telemetry attitude of every source that has streamed. */
const estimatedBySource = new Map<number, Quat>();
let selectedSource: number | null = null;
let exact: Quat | null = null;
let dirty = true;

function refreshSources(): void {
    sourceSelect.replaceChildren();
    for (const id of [...estimatedBySource.keys()].sort((a, b) => a - b)) {
        const option = document.createElement("option");
        option.value = String(id);
        option.textContent = SOURCE_NAMES.get(id) ?? `source ${id}`;
        sourceSelect.appendChild(option);
    }
    if (selectedSource !== null) {
        sourceSelect.value = String(selectedSource);
    }
}

socket.on("telemetry", (message: HubMessage) => {
    const q = asQuat(message["attitudeQuat"]);
    if (q === null || !isUsable(q)) {
        return;
    }
    const source = Number(message["sourceId"]);
    const known = estimatedBySource.has(source);
    estimatedBySource.set(source, q);
    if (!known) {
        if (selectedSource === null) {
            selectedSource = source;
        }
        refreshSources();
    }
    dirty = true;
});

socket.on("simRaw", (message: HubMessage) => {
    const q = asQuat(message["attitudeQuat"]);
    if (q === null || !isUsable(q)) {
        return;
    }
    exact = q;
    dirty = true;
});

sourceSelect.addEventListener("change", () => {
    selectedSource = Number(sourceSelect.value);
    dirty = true;
});

ghostButton.addEventListener("click", () => {
    const visible = !ghostButton.classList.contains("active");
    ghostButton.classList.toggle("active", visible);
    scene.setGhostVisible(visible);
});

/* -------------------- render loop -------------------- */

function format(values: readonly number[], digits: number): string {
    return values.map((value) => value.toFixed(digits)).join("   ");
}

function update(): void {
    const estimated = selectedSource === null ? null : estimatedBySource.get(selectedSource) ?? null;
    if (estimated !== null) {
        scene.setEstimated(toRenderQuat(estimated));
        quatValue.textContent = format(estimated, 4);
        eulerValue.textContent = format(eulerDeg(estimated), 1);
    } else {
        quatValue.textContent = "no telemetry";
        eulerValue.textContent = "-";
    }
    if (exact !== null) {
        scene.setExact(toRenderQuat(exact));
    }
    errorValue.textContent =
        estimated === null || exact === null ? "-" : errorAngleDeg(estimated, exact).toFixed(2);
}

function frame(): void {
    requestAnimationFrame(frame);
    if (document.hidden) {
        return;
    }
    if (dirty) {
        update();
        dirty = false;
    }
    // The camera moves under the pointer without any message arriving, so a
    // visible page always redraws.
    scene.render();
}

update();
requestAnimationFrame(frame);
