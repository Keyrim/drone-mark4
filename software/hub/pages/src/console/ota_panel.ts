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
 * The target is a node: the panel lists the updatable nodes the node table
 * holds (the board, a desktop flight process with its emulated flash, the
 * ESP32 relay over its two partitions) and every OtaCommand names the
 * chosen one. The gateway owns the whole state
 * machine and publishes it as one OtaState on every change, so this panel
 * never derives anything of its own: it paints what the last message said.
 */

import { create } from "@bufbuild/protobuf";

import { GatewayMessageSchema, type OtaState, OtaCommand_Op } from "../gen/gateway_pb";
import { NodeKind } from "../gen/mark4_pb";
import type { GatewaySocket } from "../shared/gateway_socket";
import { type NodeModel, type NodeView, isUpdatable, nodeLabel } from "../shared/nodes";
import {
    IDLE_OTA,
    TERMINAL,
    identityText,
    otaActions,
    otaRunning,
    phaseLabel,
    phaseTone,
    progressPercent,
    progressText,
    slotLetter,
    slotStateName,
    slotTone,
    verdictText,
} from "./ota";

/** How often the panel asks the board what it runs while idle [ms]. */
const REFRESH_MS = 3000;

/** How long a start stays armed waiting for its second click [ms]. */
const CONFIRM_MS = 4000;

/** How long a verdict stays on screen before the session block folds [ms]. */
const VERDICT_MS = 10000;

export class OtaPanel {
    readonly root: HTMLElement;
    private state: OtaState = IDLE_OTA;
    /** Node id the operator picked, 0 = none. */
    private target = 0;
    /** True once the operator has typed a path, which then wins over the gateway's. */
    private pathEdited = false;
    private readonly targetSelect: HTMLSelectElement;
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
    private verdictPhase = -1;

    constructor(
        private readonly socket: GatewaySocket,
        private readonly nodes: NodeModel,
        private readonly notify: (text: string, ok: boolean) => void
    ) {
        this.root = document.createElement("section");
        this.root.className = "panel ota-panel";

        const bar = document.createElement("div");
        bar.className = "panel-bar";
        const title = document.createElement("b");
        title.textContent = "Firmware";
        bar.appendChild(title);
        this.targetSelect = document.createElement("select");
        this.targetSelect.className = "config-select";
        this.targetSelect.title = "the node to update";
        this.targetSelect.addEventListener("change", () => {
            this.target = Number(this.targetSelect.value);
            this.refresh();
            this.paint();
        });
        bar.appendChild(this.targetSelect);
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
        this.startButton = this.button(this.sessionIdle, "Update", "btn danger", () => this.start());
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
            this.ask(OtaCommand_Op.ABORT, "abort");
        });
        this.sessionLive.appendChild(progressRow);
        this.verdictLine = document.createElement("div");
        this.verdictLine.className = "panel-body ota-verdict";
        this.sessionLive.appendChild(this.verdictLine);
        session.appendChild(this.sessionLive);

        this.root.appendChild(session);

        socket.on("otaState", (state) => {
            this.state = state;
            if (state.targetNode !== 0 && !otaRunning(state) && this.target === 0) {
                // The gateway remembers the last target across tabs
                this.target = state.targetNode;
            }
            if (!this.pathEdited && state.bundle !== undefined && state.bundle.path !== "") {
                this.bundleInput.value = state.bundle.path;
            }
            this.paint();
        });
        nodes.onChange((list) => this.paintTargets(list));
        // A board that was just plugged in has slots to show, not an event:
        // the question is asked on a slow timer whenever no session runs -
        // during one, the gateway polls the board itself.
        setInterval(() => this.refresh(), REFRESH_MS);
        this.paint();
    }

    /** The node the commands go to is alive in the table. */
    private boardLinked(): boolean {
        return this.target !== 0 && this.nodes.get(this.target) !== undefined;
    }

    private paintTargets(list: NodeView[]): void {
        const targets = list.filter(isUpdatable);
        if (this.target === 0 && targets.length > 0) {
            this.target = (targets.find((node) => node.kind === NodeKind.FIRMWARE) ?? targets[0]!).id;
        }
        this.targetSelect.replaceChildren();
        if (targets.length === 0) {
            const option = document.createElement("option");
            option.value = "0";
            option.textContent = "no updatable node on the network";
            this.targetSelect.appendChild(option);
        }
        for (const node of targets) {
            const option = document.createElement("option");
            option.value = String(node.id);
            option.textContent = nodeLabel(node);
            this.targetSelect.appendChild(option);
        }
        this.targetSelect.value = String(this.target);
        this.paint();
    }

    /** Asks the board what it runs, without bothering the operator on failure. */
    private refresh(): void {
        if (!this.boardLinked() || otaRunning(this.state)) {
            return;
        }
        void this.socket.request(this.command(OtaCommand_Op.STATUS_REQUEST)).catch(() => undefined);
    }

    private command(op: OtaCommand_Op, bundlePath = "") {
        return create(GatewayMessageSchema, {
            body: { case: "otaCommand", value: { op, targetNode: this.target, bundlePath } },
        });
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
        this.ask(OtaCommand_Op.START, "update", this.bundleInput.value);
        this.paint();
    }

    private ask(op: OtaCommand_Op, what: string, bundlePath = ""): void {
        void this.socket
            .request(this.command(op, bundlePath))
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
            this.verdictPhase = -1;
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
        const linked = this.boardLinked();
        const actions = otaActions(state, linked);
        const running = otaRunning(state);

        this.paintSlots(running, linked);

        // The session block: live while a session runs or its verdict is
        // fresh, folded to the one idle row otherwise.
        const live = running || this.verdictVisible();
        this.sessionLive.style.display = live ? "" : "none";
        this.sessionIdle.style.display = live ? "none" : "";

        const bundle = state.bundle;
        this.bundleLine.textContent =
            bundle !== undefined && bundle.loaded ? identityText(bundle.buildEpoch, bundle.gitHash) : "";
        this.bundleInput.disabled = !actions.editBundle;
        const armed = Date.now() < this.startArmedUntil;
        this.startButton.disabled = !actions.start;
        this.startButton.textContent = armed ? "Update: click again" : "Update";
        this.startButton.classList.toggle("active", armed);
        this.targetSelect.disabled = running;

        this.phaseChip.textContent = phaseLabel(state.phase);
        const tone = phaseTone(state);
        this.phaseChip.className = tone === "" ? "dw-phase" : `dw-phase ${tone}`;
        this.abortButton.disabled = !actions.abort;
        this.barFill.style.width = `${progressPercent(state)}%`;
        this.barText.textContent = progressText(state);

        const verdict = verdictText(state);
        this.verdictLine.textContent = verdict;
        this.verdictLine.style.display = verdict === "" ? "none" : "";
        this.verdictLine.className =
            "panel-body ota-verdict" +
            (tone === "good" ? " cell-ok" : tone === "bad" ? " cell-bad" : "");
    }

    /** Repaints the slot rows, the always-true block. */
    private paintSlots(running: boolean, linked: boolean): void {
        this.slotRows.textContent = "";
        const board = this.state.board;
        if (board === undefined || !board.seen || board.slots.length === 0) {
            const empty = document.createElement("div");
            empty.className = "panel-body panel-note";
            empty.textContent = linked ? "asking the board..." : "pick a drone to update";
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
            const tone = slotTone(slot.state);
            chip.className = tone === "" ? "dw-phase" : `dw-phase ${tone}`;
            chip.textContent = slotStateName(slot.state);
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
            if (index !== board.runningSlot && slotStateName(slot.state) === "valid") {
                const revert = this.button(row, "Revert to this", "btn", () => {
                    this.ask(OtaCommand_Op.REVERT, "revert");
                });
                revert.disabled = !linked || running;
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
