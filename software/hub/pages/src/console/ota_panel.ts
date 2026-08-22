/**
 * The firmware update panel of the control page: what the board runs against
 * what the bundle holds, one button to send it, the phase and the progress
 * while it goes, and the verdict when it is over.
 *
 * The hub owns the whole state machine and publishes it as one `ota` message
 * on every change, so this panel never derives anything of its own: it paints
 * what the last message said. It asks for a fresh board status on a slow
 * timer while nothing is running, because the version of a board that was
 * just plugged in is a question, not an event.
 */

import type { HubMessage, HubSocket } from "../shared/hub_socket";
import {
    IDLE_OTA,
    identityText,
    otaActions,
    phaseLabel,
    phaseTone,
    progressText,
    readOtaState,
    slotLetter,
    slotsText,
    verdictText,
    type OtaState,
} from "./ota";

/** How often the panel asks the board what it runs while idle [ms]. */
const REFRESH_MS = 3000;

/** How long a start stays armed waiting for its second click [ms]. */
const CONFIRM_MS = 4000;

export class OtaPanel {
    readonly root: HTMLElement;
    private state: OtaState = IDLE_OTA;
    private boardLinked = false;
    /** True once the operator has typed a path, which then wins over the hub's. */
    private pathEdited = false;
    private readonly phaseChip: HTMLElement;
    private readonly boardLine: HTMLElement;
    private readonly bundleLine: HTMLElement;
    private readonly slotLine: HTMLElement;
    private readonly bundleInput: HTMLInputElement;
    private readonly startButton: HTMLButtonElement;
    private readonly abortButton: HTMLButtonElement;
    private readonly confirmButton: HTMLButtonElement;
    private readonly revertButton: HTMLButtonElement;
    private readonly autoInput: HTMLInputElement;
    private readonly barFill: HTMLElement;
    private readonly barText: HTMLElement;
    private readonly verdictLine: HTMLElement;
    private startArmedUntil = 0;

    constructor(
        private readonly socket: HubSocket,
        private readonly notify: (text: string, ok: boolean) => void
    ) {
        this.root = document.createElement("section");
        this.root.className = "panel ota-panel";

        const bar = document.createElement("div");
        bar.className = "panel-bar";
        const title = document.createElement("b");
        title.textContent = "Firmware update";
        bar.appendChild(title);
        this.phaseChip = document.createElement("span");
        this.phaseChip.className = "dw-phase";
        this.phaseChip.textContent = "idle";
        bar.appendChild(this.phaseChip);
        const spacer = document.createElement("span");
        spacer.className = "bar-grow";
        bar.appendChild(spacer);

        const auto = document.createElement("label");
        auto.className = "switch-row";
        auto.title =
            "on: the hub confirms on its own once the new image has answered for a few seconds";
        const autoCaption = document.createElement("span");
        autoCaption.textContent = "auto confirm";
        this.autoInput = document.createElement("input");
        this.autoInput.type = "checkbox";
        this.autoInput.checked = true;
        this.autoInput.addEventListener("change", () => {
            this.ask(
                { type: "otaConfig", autoConfirm: this.autoInput.checked },
                this.autoInput.checked ? "auto confirm" : "manual confirm"
            );
        });
        const autoSlider = document.createElement("span");
        autoSlider.className = "switch";
        auto.appendChild(autoCaption);
        auto.appendChild(this.autoInput);
        auto.appendChild(autoSlider);
        bar.appendChild(auto);
        this.root.appendChild(bar);

        // The two identities, side by side: this is the whole "what is about
        // to replace what" question the operator has to answer before
        // clicking anything.
        const identities = document.createElement("div");
        identities.className = "ota-identities";
        this.boardLine = this.identityCell(identities, "board");
        this.bundleLine = this.identityCell(identities, "bundle");
        this.root.appendChild(identities);

        this.slotLine = document.createElement("div");
        this.slotLine.className = "panel-body panel-note";
        this.root.appendChild(this.slotLine);

        const startRow = document.createElement("div");
        startRow.className = "panel-body";
        this.bundleInput = document.createElement("input");
        this.bundleInput.className = "config-title ota-path";
        this.bundleInput.title = "the .ota bundle to send; the build output is the default";
        this.bundleInput.addEventListener("input", () => {
            this.pathEdited = true;
        });
        startRow.appendChild(this.bundleInput);
        this.startButton = this.button(startRow, "Update", "btn danger", () => this.start());
        this.abortButton = this.button(startRow, "Abort", "btn", () => {
            this.ask({ type: "otaAbort" }, "abort");
        });
        this.root.appendChild(startRow);

        const track = document.createElement("div");
        track.className = "dw-motor-track ota-bar";
        this.barFill = document.createElement("div");
        this.barFill.className = "dw-motor-fill";
        this.barFill.style.width = "0%";
        track.appendChild(this.barFill);
        const progressRow = document.createElement("div");
        progressRow.className = "panel-body";
        progressRow.appendChild(track);
        this.barText = document.createElement("span");
        this.barText.className = "panel-note ota-bytes";
        progressRow.appendChild(this.barText);
        this.root.appendChild(progressRow);

        this.verdictLine = document.createElement("div");
        this.verdictLine.className = "panel-body ota-verdict";
        this.root.appendChild(this.verdictLine);

        const endRow = document.createElement("div");
        endRow.className = "panel-body";
        this.confirmButton = this.button(endRow, "Confirm", "btn", () => {
            this.ask({ type: "otaConfirm" }, "confirm");
        });
        this.revertButton = this.button(endRow, "Revert", "btn", () => {
            this.ask({ type: "otaRevert" }, "revert");
        });
        this.root.appendChild(endRow);

        socket.on("ota", (message: HubMessage) => this.onOta(message));
        socket.on("status", (message: HubMessage) => {
            this.boardLinked = message["serialOpen"] === true;
            this.paint();
        });
        // A board that was just plugged in has a version, not an event: the
        // question is asked on a slow timer, and only while nothing is
        // running - during a session the hub polls the board itself.
        setInterval(() => this.refresh(), REFRESH_MS);
        this.paint();
    }

    /** Asks the board what it runs, without bothering the operator on failure. */
    private refresh(): void {
        if (!this.boardLinked || this.state.phase !== "idle") {
            return;
        }
        void this.socket.request({ type: "otaStatus" }).catch(() => undefined);
    }

    private onOta(message: HubMessage): void {
        this.state = readOtaState(message);
        if (!this.pathEdited && this.state.bundle.path !== "") {
            this.bundleInput.value = this.state.bundle.path;
        }
        this.autoInput.checked = this.state.autoConfirm;
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
                // Success needs no toast: the ota message says what happened,
                // and it says it for as long as the operator looks at it.
            })
            .catch((error: unknown) => this.notify(`${what}: ${String(error)}`, false));
    }

    private paint(): void {
        const state = this.state;
        const actions = otaActions(state, this.boardLinked);

        this.phaseChip.textContent = phaseLabel(state.phase);
        const tone = phaseTone(state);
        this.phaseChip.className = tone === "" ? "dw-phase" : `dw-phase ${tone}`;

        this.boardLine.textContent = state.board.seen
            ? `${identityText(state.board.version, state.board.gitHash)} from slot ${slotLetter(
                  state.board.runningSlot
              )}`
            : this.boardLinked
              ? "asking..."
              : "no board link";
        this.bundleLine.textContent = state.bundle.loaded
            ? identityText(state.bundle.version, state.bundle.gitHash)
            : "not loaded yet";
        const slots = slotsText(state.board);
        this.slotLine.textContent =
            state.targetSlot >= 0
                ? `slots: ${slots} | filling slot ${slotLetter(state.targetSlot)}`
                : slots === ""
                  ? ""
                  : `slots: ${slots}`;

        this.bundleInput.disabled = !actions.editBundle;
        const armed = Date.now() < this.startArmedUntil;
        this.startButton.disabled = !actions.start;
        this.startButton.textContent = armed ? "Update: click again" : "Update";
        this.startButton.classList.toggle("active", armed);
        this.abortButton.disabled = !actions.abort;
        this.confirmButton.disabled = !actions.confirm;
        this.confirmButton.classList.toggle("active", state.confirmReady);
        this.revertButton.disabled = !actions.revert;
        this.autoInput.disabled = !actions.editPolicy;

        this.barFill.style.width = `${Math.max(0, Math.min(100, state.progress.percent))}%`;
        this.barText.textContent = progressText(state);

        const verdict = verdictText(state);
        this.verdictLine.textContent = verdict;
        this.verdictLine.className =
            "panel-body ota-verdict" +
            (state.phase === "confirmed"
                ? " cell-ok"
                : state.phase === "failed" || state.phase === "rolledBack"
                  ? " cell-bad"
                  : "");
    }

    private identityCell(parent: HTMLElement, label: string): HTMLElement {
        const cell = document.createElement("span");
        cell.className = "dw-reading";
        const caption = document.createElement("span");
        caption.textContent = label;
        const value = document.createElement("b");
        value.textContent = "-";
        cell.appendChild(caption);
        cell.appendChild(value);
        parent.appendChild(cell);
        return value;
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
