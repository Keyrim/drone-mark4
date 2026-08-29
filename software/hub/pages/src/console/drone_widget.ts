/**
 * One connected drone, one widget: the color it wears in the 3D view, the
 * observation block every nature shares (phase, throw detector, altitude,
 * vertical, apex, motors), and the controls its nature calls for.
 *
 * A real or simulated drone gets the transmitter: kill, arm and mode are
 * switches, the throttle is a slider, and the widget streams the RC state
 * at TICK_MS from the first interaction on. There is no engage ritual: the
 * silence fail-safe of the drone itself covers a closed tab or a frozen
 * browser, and hiding the page flips the kill switch on.
 *
 * The simulated drone also carries the scenario block, and both pilotable
 * natures carry the tuning table, folded until needed.
 */

import type { HubMessage, HubSocket } from "../shared/hub_socket";
import { FLIGHT_PHASE_NAMES, sourceColor } from "../shared/series";
import {
    MODE_ALTITUDE_AUTO,
    MODE_MANUAL,
    SAFE_RC,
    TICK_MS,
    clamp01,
    rcPayload,
    type RcState,
} from "./rc";
import { TuningPanel } from "./tuning";

const THROW_STATE_NAMES = ["idle", "thrust", "ballistic"];

/** FlightPhase values the chip colors: recovered, and latched cutoff. */
const PHASE_HOVER = 5;
const PHASE_CUTOFF = 6;

/** StreamSource kinds, the nature of the drone behind the widget. */
const KIND_FIRMWARE = 1;
const KIND_DRONE_SIM = 2;

/** How often the readout repaints: telemetry lands far faster than eyes read. */
const READOUT_MS = 100;

/**
 * Telemetry silence after which the platform is shown as not ready [ms].
 * An announced drone that streams nothing has no working sensor pipeline:
 * the sim without its plant, a board with a dead IMU.
 */
const NOT_READY_MS = 1500;

/** How long a reboot stays armed waiting for its second click [ms]. */
const CONFIRM_MS = 4000;

/** Safe frames sent when a widget dies, before its stream stops for good. */
const GOODBYE_FRAMES = 2;

const MOTOR_COUNT = 4;

/** What the widget needs from the page around it. */
export interface WidgetHooks {
    notify(text: string, ok: boolean): void;
    ask(payload: Record<string, unknown>, what: string): void;
}

export class DroneWidget {
    readonly root: HTMLElement;
    private state: RcState = SAFE_RC;
    private timer: ReturnType<typeof setInterval> | null = null;
    private latest: HubMessage | null = null;
    private latestAtMs = 0;
    private readonly repaint: ReturnType<typeof setInterval>;
    private readonly phaseChip: HTMLElement;
    private readonly readings = new Map<string, HTMLElement>();
    private readonly motorFills: HTMLElement[] = [];
    private readonly motorValues: HTMLElement[] = [];
    private killInput: HTMLInputElement | null = null;
    private readonly onHide = (): void => {
        // A pilot who cannot see the drone is not piloting it
        if (document.visibilityState === "hidden") {
            this.killNow();
        }
    };

    /** Park the transmitter safe: kill on, streamed at once if streaming. */
    killNow(): void {
        if (this.timer !== null && !this.state.kill) {
            this.state = { ...this.state, kill: true };
            if (this.killInput !== null) {
                this.killInput.checked = true;
            }
            this.send();
        }
    }

    constructor(
        private readonly socket: HubSocket,
        readonly kind: number,
        readonly kindName: string,
        viaSerial: boolean,
        private readonly hooks: WidgetHooks
    ) {
        this.root = document.createElement("section");
        this.root.className = "panel drone-widget";
        this.root.style.borderLeft = `3px solid ${sourceColor(kind)}`;

        const head = document.createElement("div");
        head.className = "panel-bar";
        const dot = document.createElement("span");
        dot.className = "lane-dot";
        dot.style.background = sourceColor(kind);
        head.appendChild(dot);
        const name = document.createElement("b");
        name.textContent = kindName;
        head.appendChild(name);
        const transport = document.createElement("span");
        transport.className = "panel-note";
        transport.textContent = viaSerial ? "serial" : "udp";
        head.appendChild(transport);
        const spacer = document.createElement("span");
        spacer.className = "bar-grow";
        head.appendChild(spacer);
        if (kind === KIND_FIRMWARE) {
            head.appendChild(this.rebootButton());
        }
        this.root.appendChild(head);

        // Observation, the same whatever the nature
        const observe = document.createElement("div");
        observe.className = "dw-obs";
        this.phaseChip = document.createElement("span");
        this.phaseChip.className = "dw-phase";
        this.phaseChip.textContent = "-";
        observe.appendChild(this.phaseChip);
        for (const label of ["throw", "throws", "alt", "vz", "apex"]) {
            const cell = document.createElement("span");
            cell.className = "dw-reading";
            const title = document.createElement("span");
            title.textContent = label;
            const value = document.createElement("b");
            value.textContent = "-";
            cell.appendChild(title);
            cell.appendChild(value);
            observe.appendChild(cell);
            this.readings.set(label, value);
        }
        this.root.appendChild(observe);

        const motors = document.createElement("div");
        motors.className = "dw-motors";
        for (let i = 0; i < MOTOR_COUNT; ++i) {
            const row = document.createElement("div");
            row.className = "dw-motor";
            const motorName = document.createElement("span");
            motorName.textContent = `M${i + 1}`;
            const track = document.createElement("div");
            track.className = "dw-motor-track";
            const fill = document.createElement("div");
            fill.className = "dw-motor-fill";
            track.appendChild(fill);
            const value = document.createElement("b");
            value.textContent = "-";
            row.appendChild(motorName);
            row.appendChild(track);
            row.appendChild(value);
            motors.appendChild(row);
            this.motorFills.push(fill);
            this.motorValues.push(value);
        }
        this.root.appendChild(motors);

        // Controls, by nature
        if (kind === KIND_FIRMWARE || kind === KIND_DRONE_SIM) {
            this.root.appendChild(this.transmitter());
            if (kind === KIND_DRONE_SIM) {
                this.root.appendChild(this.scenarioBlock());
            }
            this.root.appendChild(this.tuningBlock());
        }

        document.addEventListener("visibilitychange", this.onHide);
        this.repaint = setInterval(() => this.paint(), READOUT_MS);
    }

    /** Telemetry of THIS drone, routed by the page. */
    onTelemetry(message: HubMessage): void {
        this.latest = message;
        this.latestAtMs = Date.now();
    }

    /** The widget is leaving: park the drone safe and stop streaming. */
    destroy(): void {
        document.removeEventListener("visibilitychange", this.onHide);
        clearInterval(this.repaint);
        if (this.timer !== null) {
            clearInterval(this.timer);
            this.timer = null;
            // The safe state goes out twice in case one datagram is lost,
            // and then the stream stops: the stop is itself the fail-safe.
            this.state = SAFE_RC;
            for (let i = 0; i < GOODBYE_FRAMES; ++i) {
                this.send();
            }
        }
        this.root.remove();
    }

    /* -------------------- transmitter -------------------- */

    private transmitter(): HTMLElement {
        const block = document.createElement("div");
        block.className = "dw-rc";

        const kill = this.switchRow("kill", true, (on) => {
            this.apply({ ...this.state, kill: on });
        });
        kill.input.classList.add("danger");
        this.killInput = kill.input;
        block.appendChild(kill.root);

        block.appendChild(
            this.switchRow("arm", false, (on) => {
                this.apply({ ...this.state, arm: on });
            }).root
        );

        const mode = this.switchRow("altitude auto", false, (on) => {
            this.apply({ ...this.state, mode: on ? MODE_ALTITUDE_AUTO : MODE_MANUAL });
        });
        mode.root.title = "off = manual (stick is the collective), on = altitude auto";
        block.appendChild(mode.root);

        const throttle = document.createElement("label");
        throttle.className = "dw-throttle";
        const caption = document.createElement("span");
        caption.textContent = "throttle";
        const slider = document.createElement("input");
        slider.type = "range";
        slider.min = "0";
        slider.max = "100";
        slider.value = "0";
        const value = document.createElement("b");
        value.textContent = "0%";
        slider.addEventListener("input", () => {
            value.textContent = `${slider.value}%`;
            this.apply({ ...this.state, throttle: clamp01(Number(slider.value) / 100) });
        });
        throttle.appendChild(caption);
        throttle.appendChild(slider);
        throttle.appendChild(value);
        block.appendChild(throttle);

        return block;
    }

    /** New RC state from a control; the first one starts the stream. */
    private apply(state: RcState): void {
        this.state = state;
        if (this.timer === null) {
            this.timer = setInterval(() => this.send(), TICK_MS);
        }
        this.send();
    }

    private send(): void {
        this.socket.send(rcPayload(this.state, this.kindName));
    }

    private switchRow(
        label: string,
        checked: boolean,
        onChange: (on: boolean) => void
    ): { root: HTMLElement; input: HTMLInputElement } {
        const root = document.createElement("label");
        root.className = "switch-row";
        const caption = document.createElement("span");
        caption.textContent = label;
        const input = document.createElement("input");
        input.type = "checkbox";
        input.checked = checked;
        input.addEventListener("change", () => onChange(input.checked));
        const slider = document.createElement("span");
        slider.className = "switch";
        root.appendChild(caption);
        root.appendChild(input);
        root.appendChild(slider);
        return { root, input };
    }

    /* -------------------- nature-specific blocks -------------------- */

    private rebootButton(): HTMLButtonElement {
        const button = document.createElement("button");
        button.className = "btn danger";
        button.textContent = "Reboot";
        // Two clicks, because a reboot in the middle of a bench run costs a run
        let armedUntil = 0;
        button.addEventListener("click", () => {
            if (Date.now() < armedUntil) {
                armedUntil = 0;
                button.textContent = "Reboot";
                button.classList.remove("active");
                this.hooks.ask({ type: "reboot", target: this.kindName }, "reboot");
                return;
            }
            armedUntil = Date.now() + CONFIRM_MS;
            button.textContent = "Reboot: click again";
            button.classList.add("active");
            setTimeout(() => {
                if (Date.now() >= armedUntil) {
                    button.textContent = "Reboot";
                    button.classList.remove("active");
                }
            }, CONFIRM_MS);
        });
        return button;
    }

    private scenarioBlock(): HTMLElement {
        const details = document.createElement("details");
        details.className = "dw-details";
        const summary = document.createElement("summary");
        summary.textContent = "Scenario";
        details.appendChild(summary);

        const fields = document.createElement("div");
        fields.className = "panel-body";
        const inputs = new Map<string, () => number>();
        const defaults: [string, number][] = [
            ["seed", 1234],
            ["throwDelayUs", 2000000],
            ["velocityMps z", 6],
            ["angularVelocityRadS z", 0],
            ["heldSeconds", 1.5],
            ["heldTiltRad", 0.3],
            ["heldAzimuthRad", 0],
            ["swingSeconds", 0.35],
        ];
        for (const [label, initial] of defaults) {
            const field = document.createElement("label");
            field.className = "field";
            const caption = document.createElement("span");
            caption.textContent = label;
            const input = document.createElement("input");
            input.type = "number";
            input.step = "any";
            input.className = "config-title";
            input.value = String(initial);
            field.appendChild(caption);
            field.appendChild(input);
            fields.appendChild(field);
            inputs.set(label, () => Number(input.value));
        }

        const buttons = document.createElement("div");
        buttons.className = "panel-body";
        for (const scenario of ["reset", "throw", "handThrow"]) {
            const button = document.createElement("button");
            button.className = "btn";
            button.textContent = scenario;
            button.addEventListener("click", () => {
                const read = (label: string): number => inputs.get(label)?.() ?? 0;
                const payload: Record<string, unknown> = {
                    type: "simScenario",
                    target: this.kindName,
                    scenario,
                    seed: read("seed"),
                };
                if (scenario !== "reset") {
                    payload["velocityMps"] = [0, 0, read("velocityMps z")];
                    payload["angularVelocityRadS"] = [0, 0, read("angularVelocityRadS z")];
                }
                if (scenario === "throw") {
                    payload["throwDelayUs"] = read("throwDelayUs");
                }
                if (scenario === "handThrow") {
                    payload["heldSeconds"] = read("heldSeconds");
                    payload["heldTiltRad"] = read("heldTiltRad");
                    payload["heldAzimuthRad"] = read("heldAzimuthRad");
                    payload["swingSeconds"] = read("swingSeconds");
                }
                this.hooks.ask(payload, scenario);
            });
            buttons.appendChild(button);
        }
        details.appendChild(buttons);
        details.appendChild(fields);
        return details;
    }

    private tuningBlock(): HTMLElement {
        const details = document.createElement("details");
        details.className = "dw-details";
        const summary = document.createElement("summary");
        summary.textContent = "Tuning";
        details.appendChild(summary);
        const tuning = new TuningPanel(this.socket, () => this.kindName, (text, ok) =>
            this.hooks.notify(text, ok)
        );
        let started = false;
        details.addEventListener("toggle", () => {
            if (details.open && !started) {
                started = true;
                tuning.start();
            }
        });
        details.appendChild(tuning.root);
        return details;
    }

    /* -------------------- readout -------------------- */

    private paint(): void {
        // Announced but silent on telemetry = no working sensor pipeline
        const silent = this.latest === null || Date.now() - this.latestAtMs > NOT_READY_MS;
        this.root.classList.toggle("silent", silent);
        if (silent) {
            this.phaseChip.textContent = "waiting platform";
            this.phaseChip.className = "dw-phase warn";
            return;
        }
        const m = this.latest as HubMessage;
        const phase = Number(m["flightPhase"]);
        this.phaseChip.textContent = FLIGHT_PHASE_NAMES[phase] ?? String(phase);
        this.phaseChip.className =
            "dw-phase" + (phase === PHASE_HOVER ? " good" : phase === PHASE_CUTOFF ? " bad" : "");
        const set = (label: string, text: string): void => {
            this.readings.get(label)!.textContent = text;
        };
        set("throw", THROW_STATE_NAMES[Number(m["throwState"])] ?? String(m["throwState"]));
        set("throws", String(m["throwCount"]));
        set("alt", `${Number(m["altitudeM"]).toFixed(2)} m`);
        set("vz", `${Number(m["verticalVelocityMps"]).toFixed(2)} m/s`);
        set("apex", `${Number(m["apexAltitudeM"]).toFixed(2)} m`);
        const motorValues = (m["motor"] as number[]) ?? [];
        this.motorFills.forEach((fill, i) => {
            const value = motorValues[i];
            fill.style.width = `${clamp01(value ?? 0) * 100}%`;
            this.motorValues[i]!.textContent = value === undefined ? "-" : value.toFixed(2);
        });
    }
}
