/**
 * The firmware update panel of the control page, in two visual blocks with
 * two lifetimes:
 *
 * - the board block is always true: one row per firmware slot with its
 *   state, the identity of the image in it, which slot runs and which one
 *   the bootloader prefers, plus the revert action on the slot it would
 *   boot instead;
 * - the session block is operational and short-lived: bundle choice,
 *   progress and phase while an update runs, the verdict when it is over,
 *   and the verdict dismisses itself because the board block already tells
 *   the durable outcome.
 *
 * The hub owns the whole state machine and publishes it as one `ota`
 * message on every change, so this panel never derives anything of its own:
 * it paints what the last message said. It asks for a fresh board status on
 * a slow timer while nothing is running, because the slot rows must stay
 * true whether or not anyone updates anything.
 */

import type { HubMessage, HubSocket } from "../shared/hub_socket";
import {
    IDLE_OTA,
    identityText,
    otaActions,
    otaRunning,
    phaseLabel,
    phaseTone,
    progressText,
    readOtaState,
    slotLetter,
    slotTone,
    verdictText,
    type OtaState,
} from "./ota";

/** How often the panel asks the board what it runs while idle [ms]. */
const REFRESH_MS = 3000;

/** How long a start stays armed waiting for its second click [ms]. */
const CONFIRM_MS = 4000;

/** How long a verdict stays on screen before the session block folds [ms]. */
const VERDICT_MS = 10000;

/** Phases that end a session one way or the other. */
const TERMINAL = new Set<string>(["confirmed", "rolledBack", "failed"]);

export class OtaPanel {
    readonly root: HTMLElement;
    private state: OtaState = IDLE_OTA;
    private boardLinked = false;
    /** True once the operator has typed a path, which then wins over the hub's. */
    private pathEdited = false;
    private readonly slotRows: HTMLElement;
    private readonly sessionIdle: HTMLElement;
    private readonly sessionLive: HTMLElement;
    private readonly phaseChip: HTMLElement;
    private readonly bundleLine: HTMLElement;
    private readonly bundleInput: HTMLInputElement;
    private readonly startButton: HTMLButtonElement;
    private readonly abortButton: HTMLButtonElement;
    private readonly barFill: HTMLElement;
    private readonly barText: HTMLElement;
    private readonly verdictLine: HTMLElement;
    private startArmedUntil = 0;
    /** When the current verdict appeared, 0 while none is on screen. */
    private verdictShownAt = 0;
    /** The terminal phase the timestamp above belongs to. */
    private verdictPhase = "";

    constructor(
        private readonly socket: HubSocket,
        private readonly notify: (text: string, ok: boolean) => void
    ) {
        this.root = document.createElement("section");
        this.root.className = "panel ota-panel";

        const bar = document.createElement("div");
        bar.className = "panel-bar";
        const title = document.createElement("b");
        title.textContent = "Firmware";
        bar.appendChild(title);
        this.root.appendChild(bar);

        // Block one: the slots, the durable truth of the board.
        this.slotRows = document.createElement("div");
        this.slotRows.className = "ota-slots";
        this.root.appendChild(this.slotRows);

        // Block two: the update session, folded to one row while nothing
        // runs and nothing just finished.
        const session = document.createElement("div");
        session.className = "ota-session";

        this.sessionIdle = document.createElement("div");
        this.sessionIdle.className = "panel-body";
        this.bundleInput = document.createElement("input");
        this.bundleInput.className = "config-title ota-path";
        this.bundleInput.title = "the .ota bundle to send; the build output is the default";
        this.bundleInput.addEventListener("input", () => {
            this.pathEdited = true;
        });
        this.sessionIdle.appendChild(this.bundleInput);
        this.bundleLine = document.createElement("span");
        this.bundleLine.className = "panel-note ota-bundle-id";
        this.sessionIdle.appendChild(this.bundleLine);
        this.startButton = this.button(this.sessionIdle, "Update", "btn danger", () =>
            this.start()
        );
        session.appendChild(this.sessionIdle);

        this.sessionLive = document.createElement("div");
        const progressRow = document.createElement("div");
        progressRow.className = "panel-body";
        this.phaseChip = document.createElement("span");
        this.phaseChip.className = "dw-phase";
        progressRow.appendChild(this.phaseChip);
        const track = document.createElement("div");
        track.className = "dw-motor-track ota-bar";
        this.barFill = document.createElement("div");
        this.barFill.className = "dw-motor-fill";
        this.barFill.style.width = "0%";
        track.appendChild(this.barFill);
        progressRow.appendChild(track);
        this.barText = document.createElement("span");
        this.barText.className = "panel-note ota-bytes";
        progressRow.appendChild(this.barText);
        this.abortButton = this.button(progressRow, "Abort", "btn", () => {
            this.ask({ type: "otaAbort" }, "abort");
        });
        this.sessionLive.appendChild(progressRow);
        this.verdictLine = document.createElement("div");
        this.verdictLine.className = "panel-body ota-verdict";
        this.sessionLive.appendChild(this.verdictLine);
        session.appendChild(this.sessionLive);

        this.root.appendChild(session);

        socket.on("ota", (message: HubMessage) => this.onOta(message));
        socket.on("status", (message: HubMessage) => {
            // The board is linked when it is THE connected drone and alive:
            // it is a node like the others, there is no link to be open.
            const connection = (message["connection"] ?? {}) as Record<string, unknown>;
            this.boardLinked = connection["kindName"] === "firmware" && connection["live"] === true;
            this.paint();
        });
        // A board that was just plugged in has slots to show, not an event:
        // the question is asked on a slow timer whenever no session runs -
        // during one, the hub polls the board itself.
        setInterval(() => this.refresh(), REFRESH_MS);
        this.paint();
    }

    /** Asks the board what it runs, without bothering the operator on failure. */
    private refresh(): void {
        if (!this.boardLinked || otaRunning(this.state)) {
            return;
        }
        void this.socket.request({ type: "otaStatus" }).catch(() => undefined);
    }

    private onOta(message: HubMessage): void {
        this.state = readOtaState(message);
        if (!this.pathEdited && this.state.bundle.path !== "") {
            this.bundleInput.value = this.state.bundle.path;
        }
        this.paint();
    }

    /**
     * Two clicks, like the reboot button: an update reboots the drone and
     * spends its one trial attempt, so it is not a thing to start by mistake.
     */
    private start(): void {
        if (Date.now() >= this.startArmedUntil) {
            this.startArmedUntil = Date.now() + CONFIRM_MS;
            this.paint();
            setTimeout(() => this.paint(), CONFIRM_MS);
            return;
        }
        this.startArmedUntil = 0;
        const payload: Record<string, unknown> = { type: "otaStart" };
        if (this.bundleInput.value !== "") {
            payload["bundle"] = this.bundleInput.value;
        }
        this.ask(payload, "update");
        this.paint();
    }

    private ask(payload: Record<string, unknown>, what: string): void {
        void this.socket
            .request(payload)
            .then((ack) => {
                if (!ack.ok) {
                    this.notify(`${what}: ${ack.error}`, false);
                }
                // Success needs no toast: the panel says what happened.
            })
            .catch((error: unknown) => this.notify(`${what}: ${String(error)}`, false));
    }

    /** True while the verdict of a just-finished session is on screen. */
    private verdictVisible(): boolean {
        if (!TERMINAL.has(this.state.phase)) {
            this.verdictShownAt = 0;
            this.verdictPhase = "";
            return false;
        }
        if (this.verdictPhase !== this.state.phase) {
            // A fresh verdict: show it, and schedule the fold.
            this.verdictPhase = this.state.phase;
            this.verdictShownAt = Date.now();
            setTimeout(() => this.paint(), VERDICT_MS);
        }
        return Date.now() - this.verdictShownAt < VERDICT_MS;
    }

    private paint(): void {
        const state = this.state;
        const actions = otaActions(state, this.boardLinked);
        const running = otaRunning(state);

        this.paintSlots(running);

        // The session block: live while a session runs or its verdict is
        // fresh, folded to the one idle row otherwise.
        const live = running || this.verdictVisible();
        this.sessionLive.style.display = live ? "" : "none";
        this.sessionIdle.style.display = live ? "none" : "";

        this.bundleLine.textContent = state.bundle.loaded
            ? identityText(state.bundle.buildEpoch, state.bundle.gitHash)
            : "";
        this.bundleInput.disabled = !actions.editBundle;
        const armed = Date.now() < this.startArmedUntil;
        this.startButton.disabled = !actions.start;
        this.startButton.textContent = armed ? "Update: click again" : "Update";
        this.startButton.classList.toggle("active", armed);

        this.phaseChip.textContent = phaseLabel(state.phase);
        const tone = phaseTone(state);
        this.phaseChip.className = tone === "" ? "dw-phase" : `dw-phase ${tone}`;
        this.abortButton.disabled = !actions.abort;
        this.barFill.style.width = `${Math.max(0, Math.min(100, state.progress.percent))}%`;
        this.barText.textContent = progressText(state);

        const verdict = verdictText(state);
        this.verdictLine.textContent = verdict;
        this.verdictLine.style.display = verdict === "" ? "none" : "";
        this.verdictLine.className =
            "panel-body ota-verdict" +
            (state.phase === "confirmed"
                ? " cell-ok"
                : state.phase === "failed" || state.phase === "rolledBack"
                  ? " cell-bad"
                  : "");
    }

    /** Repaints the slot rows, the always-true block. */
    private paintSlots(running: boolean): void {
        this.slotRows.textContent = "";
        const board = this.state.board;
        if (!board.seen || board.slots.length === 0) {
            const empty = document.createElement("div");
            empty.className = "panel-body panel-note";
            empty.textContent = this.boardLinked ? "asking the board..." : "no board link";
            this.slotRows.appendChild(empty);
            return;
        }
        board.slots.forEach((slot, index) => {
            const row = document.createElement("div");
            row.className = "panel-body ota-slot";

            const run = document.createElement("span");
            run.className = "ota-slot-run";
            run.textContent = index === board.runningSlot ? ">" : "";
            run.title = index === board.runningSlot ? "the firmware executes from this slot" : "";
            row.appendChild(run);

            const letter = document.createElement("b");
            letter.textContent = slotLetter(index);
            row.appendChild(letter);

            const chip = document.createElement("span");
            const tone = slotTone(slot.stateName);
            chip.className = tone === "" ? "dw-phase" : `dw-phase ${tone}`;
            chip.textContent = slot.stateName;
            row.appendChild(chip);

            const identity = document.createElement("span");
            identity.className = "panel-note ota-slot-id";
            identity.textContent = identityText(slot.buildEpoch, slot.gitHash);
            row.appendChild(identity);

            if (index === board.activeSlot && index !== board.runningSlot) {
                const active = document.createElement("span");
                active.className = "panel-note";
                active.textContent = "boots next";
                active.title = "the boot metadata prefers this slot";
                row.appendChild(active);
            }

            // Revert lives on the slot it would boot: an action on durable
            // state, not on the session.
            if (index !== board.runningSlot && slot.stateName === "valid") {
                const revert = this.button(row, "Revert to this", "btn", () => {
                    this.ask({ type: "otaRevert" }, "revert");
                });
                revert.disabled = !this.boardLinked || running;
            }

            this.slotRows.appendChild(row);
        });
    }

    private button(
        parent: HTMLElement,
        label: string,
        className: string,
        onClick: () => void
    ): HTMLButtonElement {
        const button = document.createElement("button");
        button.className = className;
        button.textContent = label;
        button.addEventListener("click", onClick);
        parent.appendChild(button);
        return button;
    }
}
