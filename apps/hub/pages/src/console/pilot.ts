/**
 * Keyboard piloting: the toggle, the key bindings, the 10 Hz stream and the
 * banner that says what is going out.
 *
 * The state machine itself is in rc.ts and is pure; this file is the wiring
 * that cannot be tested without a browser, and it is deliberately thin.
 *
 * Disengaging is the safety primitive, so everything that could mean "the
 * pilot is no longer watching" ends up calling it: the toggle, Escape, the
 * window losing focus, the tab going to the background, the page unloading,
 * the hub link dropping, a tick that arrived late, and the deadman. It sends
 * the safe state twice and then stops sending: the drone's own RC timeout
 * turns that silence into a cut, which is why a browser that freezes outright
 * is covered too.
 */

import type { HubSocket } from "../shared/hub_socket";
import { MODE_ALTITUDE_AUTO, SAFE_RC, TICK_MS, keyEvent, rcPayload, rcReduce } from "./rc";
import type { RcEvent, RcState } from "./rc";

/** Window the measured stream rate is averaged over [ms]. */
const RATE_WINDOW_MS = 1000;

/** Safe frames sent on the way out, before the stream stops for good. */
const GOODBYE_FRAMES = 2;

/** Flight phase 0: the core only accepts a mode change while it is here. */
const PHASE_IDLE = 0;

export class Pilot {
    /** The engage toggle, for the toolbar. */
    readonly toggle: HTMLButtonElement;
    /** Fixed banner, only in the document while engaged. */
    private readonly banner: HTMLElement;
    private readonly readout: HTMLElement;
    private readonly warning: HTMLElement;
    private readonly buttons: HTMLButtonElement[] = [];
    private state: RcState = SAFE_RC;
    private timer: ReturnType<typeof setInterval> | null = null;
    private sentAtMs: number[] = [];
    private motors: number[] = [];
    private phase = PHASE_IDLE;
    private clients = 1;
    private readonly onKeyDown = (event: KeyboardEvent) => this.onKey(event, true);
    private readonly onKeyUp = (event: KeyboardEvent) => this.onKey(event, false);
    private readonly onBlur = () => this.apply({ type: "disengage", reason: "window lost focus" });
    private readonly onHide = () => {
        if (document.visibilityState === "hidden") {
            this.apply({ type: "disengage", reason: "tab went to the background" });
        }
    };
    private readonly onPageHide = () => this.apply({ type: "disengage", reason: "page left" });

    /**
     * @param socket link the rc messages go out on
     * @param target names the process the stream is addressed to
     * @param notify says one line to the operator
     */
    constructor(
        private readonly socket: HubSocket,
        private readonly target: () => string,
        private readonly notify: (text: string, ok: boolean) => void
    ) {
        this.toggle = document.createElement("button");
        this.toggle.className = "btn danger";
        this.toggle.textContent = "Pilot: off";
        this.toggle.addEventListener("click", () => {
            if (this.state.engaged) {
                this.apply({ type: "disengage", reason: "toggled off" });
            } else if (this.target() === "") {
                this.notify("no flight process to pilot", false);
            } else {
                this.apply({ type: "engage", atMs: Date.now() });
            }
        });

        this.banner = document.createElement("div");
        this.banner.className = "pilot-banner";
        this.readout = document.createElement("div");
        this.readout.className = "pilot-readout";
        this.warning = document.createElement("div");
        this.warning.className = "pilot-warning";
        this.banner.appendChild(this.readout);
        this.banner.appendChild(this.warning);

        socket.onState((connection) => {
            if (connection !== "open" && this.state.engaged) {
                this.apply({ type: "disengage", reason: "the hub link dropped" });
            }
        });
    }

    /** The controls the operator drives by mouse, disabled while off. */
    controls(): HTMLElement {
        const row = document.createElement("div");
        row.className = "pilot-controls";
        const buttons: [string, () => RcEvent][] = [
            ["Kill", () => ({ type: "toggleKill", atMs: Date.now() })],
            ["Arm", () => ({ type: "toggleArm", atMs: Date.now() })],
            ["Mode", () => ({ type: "toggleMode", atMs: Date.now() })],
            ["Throttle 0", () => ({ type: "setThrottle", value: 0, atMs: Date.now() })],
            ["Throttle 50%", () => ({ type: "setThrottle", value: 0.5, atMs: Date.now() })],
        ];
        for (const [label, make] of buttons) {
            const button = document.createElement("button");
            button.className = "btn pilot-button";
            button.textContent = label;
            button.disabled = true;
            button.addEventListener("click", () => this.apply(make()));
            this.buttons.push(button);
            row.appendChild(button);
        }
        const help = document.createElement("span");
        help.className = "pilot-help";
        help.textContent =
            "K kill | Space panic kill | Esc off | A arm | M mode | " +
            "Up/Down throttle (Shift = fine) | 0 or Home zero | C 50%";
        row.appendChild(help);
        return row;
    }

    /** Latest telemetry, so the banner can show what the command produced. */
    setTelemetry(motors: number[], flightPhase: number): void {
        this.motors = motors;
        this.phase = flightPhase;
    }

    /** Client count from the hub status: another tab may be streaming too. */
    setClients(clients: number): void {
        this.clients = clients;
        this.paint();
    }

    private onKey(event: KeyboardEvent, down: boolean): void {
        if (!this.state.engaged) {
            return;
        }
        const decoded = keyEvent(event.code, event.shiftKey, event.repeat, down, Date.now());
        if (decoded === null) {
            return;
        }
        // Space scrolls, arrows scroll, and a pilot key is not a page key
        event.preventDefault();
        if (decoded.type === "toggleMode" && this.phase !== PHASE_IDLE) {
            this.notify(
                "the core locks the piloting mode on leaving idle: this takes effect once it is idle again",
                false
            );
        }
        this.apply(decoded);
    }

    /** Feeds one event to the reducer and reacts to what changed. */
    private apply(event: RcEvent): void {
        const before = this.state.engaged;
        this.state = rcReduce(this.state, event);
        if (this.state.engaged && !before) {
            this.start();
        } else if (!this.state.engaged && before) {
            this.stop();
        } else if (this.state.engaged) {
            this.send();
            this.paint();
        }
    }

    private start(): void {
        addEventListener("keydown", this.onKeyDown);
        addEventListener("keyup", this.onKeyUp);
        addEventListener("blur", this.onBlur);
        addEventListener("pagehide", this.onPageHide);
        document.addEventListener("visibilitychange", this.onHide);
        document.body.appendChild(this.banner);
        this.sentAtMs = [];
        this.timer = setInterval(() => this.apply({ type: "tick", atMs: Date.now() }), TICK_MS);
        this.toggle.textContent = "Pilot: ENGAGED";
        this.toggle.classList.add("active");
        this.setControlsEnabled(true);
        this.send();
        this.paint();
    }

    private stop(): void {
        removeEventListener("keydown", this.onKeyDown);
        removeEventListener("keyup", this.onKeyUp);
        removeEventListener("blur", this.onBlur);
        removeEventListener("pagehide", this.onPageHide);
        document.removeEventListener("visibilitychange", this.onHide);
        if (this.timer !== null) {
            clearInterval(this.timer);
            this.timer = null;
        }
        // The safe state goes out twice in case one datagram is lost, and
        // then the stream stops: the stop is itself the fail-safe.
        for (let i = 0; i < GOODBYE_FRAMES; ++i) {
            this.send();
        }
        this.banner.remove();
        this.toggle.textContent = "Pilot: off";
        this.toggle.classList.remove("active");
        this.setControlsEnabled(false);
        if (this.state.reason !== "") {
            this.notify(`pilot disengaged: ${this.state.reason}`, false);
        }
    }

    private setControlsEnabled(enabled: boolean): void {
        for (const button of this.buttons) {
            button.disabled = !enabled;
        }
    }

    private send(): void {
        this.socket.send(rcPayload(this.state, this.target()));
        const now = Date.now();
        this.sentAtMs.push(now);
        while (this.sentAtMs.length > 0 && now - this.sentAtMs[0]! > RATE_WINDOW_MS) {
            this.sentAtMs.shift();
        }
    }

    private paint(): void {
        if (!this.state.engaged) {
            return;
        }
        const motors = this.motors.map((value) => value.toFixed(2)).join(" ");
        this.readout.textContent =
            `PILOT ENGAGED -> ${this.target()} | ` +
            `${this.state.kill ? "KILL" : "live"} | ${this.state.arm ? "ARMED" : "disarmed"} | ` +
            `${this.state.mode === MODE_ALTITUDE_AUTO ? "altitude auto" : "manual"} | ` +
            `cmd ${(this.state.throttle * 100).toFixed(0)}% | ` +
            `motors ${motors === "" ? "-" : motors} | ` +
            `${this.sentAtMs.length} Hz out`;
        this.warning.textContent =
            this.clients > 1
                ? `${this.clients} clients on this hub: another console could be streaming rc too`
                : "";
    }
}
