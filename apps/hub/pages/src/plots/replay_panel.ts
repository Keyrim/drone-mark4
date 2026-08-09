/**
 * Replay panel: pick one of the recordings the hub holds, open it in the
 * lanes, and read the cards that go with it. The blackbox summary and the
 * streams score both come from the hub, which is the authority on them; the
 * page only lays them out.
 */

import { fillFromBlackbox, fillFromStreams, type Filled } from "../lanes/replay";
import type { LaneConfig } from "../lanes/model";
import {
    blackboxTable,
    fileUrl,
    getCompare,
    getRecording,
    getSummary,
    listRecordings,
    type ComparePayload,
    type RecordingEntry,
    type SummaryPayload,
} from "../shared/api";
import { formatDuration } from "../lanes/timebase";

/** Points asked of the hub per stream; the lanes decimate what is left. */
const MAX_POINTS = 20000;

export class ReplayPanel {
    readonly root: HTMLElement;
    private readonly picker: HTMLSelectElement;
    private readonly cards: HTMLElement;
    private entries: RecordingEntry[] = [];

    constructor(
        private readonly defaultLanes: () => LaneConfig[],
        private readonly onLoaded: (filled: Filled, name: string) => void
    ) {
        this.root = document.createElement("div");
        this.root.className = "replay";

        const bar = document.createElement("div");
        bar.className = "replay-bar";

        this.picker = document.createElement("select");
        this.picker.className = "config-select";
        this.picker.addEventListener("change", () => {
            if (this.picker.value !== "") {
                void this.open(this.picker.value);
            }
        });
        bar.appendChild(this.picker);

        const refresh = document.createElement("button");
        refresh.className = "btn";
        refresh.textContent = "Refresh";
        refresh.addEventListener("click", () => void this.refresh());
        bar.appendChild(refresh);

        this.cards = document.createElement("div");
        this.cards.className = "cards";

        this.root.appendChild(bar);
        this.root.appendChild(this.cards);
    }

    /** Reload the listing, then open the deep-linked recording if any. */
    async refresh(): Promise<void> {
        try {
            const listing = await listRecordings();
            this.entries = listing.recordings;
        } catch (error) {
            this.entries = [];
            this.fail(String(error));
            return;
        }
        this.picker.replaceChildren();
        const head = document.createElement("option");
        head.value = "";
        head.textContent =
            this.entries.length === 0 ? "no recording on the hub" : "pick a recording...";
        this.picker.appendChild(head);
        for (const entry of this.entries) {
            const option = document.createElement("option");
            option.value = entry.name;
            const when = new Date(entry.modifiedUnixS * 1000).toISOString().replace("T", " ");
            option.textContent = `${entry.name} (${entry.kind}, ${sizeText(entry.sizeBytes)}, ${when.slice(0, 19)})`;
            this.picker.appendChild(option);
        }
        const wanted = new URLSearchParams(location.search).get("rec");
        if (wanted !== null && this.entries.some((e) => e.name === wanted)) {
            this.picker.value = wanted;
            await this.open(wanted);
        }
    }

    /** Load one recording into the lanes and repaint the cards. */
    async open(name: string): Promise<void> {
        const entry = this.entries.find((e) => e.name === name);
        this.picker.value = name;
        setDeepLink(name);
        this.cards.replaceChildren(note("loading..."));
        try {
            const payload = await getRecording(name, { maxPoints: MAX_POINTS });
            if (payload.kind === "blackbox") {
                const table = blackboxTable(payload);
                if (table === null) {
                    throw new Error("the hub sent a blackbox payload without a table");
                }
                this.onLoaded(fillFromBlackbox(table), name);
            } else {
                if (!payload.telemetry) {
                    throw new Error("the hub sent a streams payload without telemetry");
                }
                this.onLoaded(
                    fillFromStreams(payload.telemetry, payload.simRaw ?? null, this.defaultLanes()),
                    name
                );
            }
            await this.renderCards(name, entry, payload.skippedBytes);
        } catch (error) {
            this.fail(String(error));
        }
    }

    private async renderCards(
        name: string,
        entry: RecordingEntry | undefined,
        skippedBytes: number | undefined
    ): Promise<void> {
        this.cards.replaceChildren();
        const kind = entry?.kind ?? "streams";

        if (kind === "blackbox") {
            try {
                this.cards.appendChild(summaryCard(await getSummary(name)));
            } catch (error) {
                this.cards.appendChild(errorCard("summary", String(error)));
            }
        } else {
            try {
                this.cards.appendChild(compareCard(await getCompare(name)));
            } catch (error) {
                this.cards.appendChild(errorCard("compare", String(error)));
            }
        }
        if (skippedBytes !== undefined && skippedBytes > 0) {
            this.cards.appendChild(
                card("decoding", [["skipped", `${skippedBytes} B of unreadable record`]])
            );
        }
        this.cards.appendChild(downloadCard(name, entry));
    }

    private fail(text: string): void {
        this.cards.replaceChildren(errorCard("hub", text));
    }
}

function setDeepLink(name: string): void {
    const url = new URL(location.href);
    url.searchParams.set("rec", name);
    history.replaceState(null, "", url);
}

function sizeText(bytes: number): string {
    if (bytes < 1024) {
        return `${bytes} B`;
    }
    if (bytes < 1024 * 1024) {
        return `${(bytes / 1024).toFixed(1)} kB`;
    }
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function card(title: string, rows: [string, string][]): HTMLElement {
    const box = document.createElement("div");
    box.className = "card";
    const head = document.createElement("div");
    head.className = "card-title";
    head.textContent = title;
    box.appendChild(head);
    for (const [label, value] of rows) {
        const row = document.createElement("div");
        row.className = "card-row";
        const left = document.createElement("span");
        left.textContent = label;
        const right = document.createElement("b");
        right.textContent = value;
        row.appendChild(left);
        row.appendChild(right);
        box.appendChild(row);
    }
    return box;
}

function note(text: string): HTMLElement {
    const box = document.createElement("div");
    box.className = "card";
    box.textContent = text;
    return box;
}

function errorCard(title: string, text: string): HTMLElement {
    const box = card(title, [["failed", text]]);
    box.classList.add("bad");
    return box;
}

function summaryCard(summary: SummaryPayload): HTMLElement {
    return card("blackbox", [
        ["records", String(summary.records)],
        ["duration", formatDuration(summary.durationS)],
        ["rate", `${summary.rateHz.toFixed(1)} Hz`],
        [
            "accel norm",
            `${summary.accelNormG.min.toFixed(2)} to ${summary.accelNormG.max.toFixed(2)} g`,
        ],
        ["kill engaged", `${summary.killRecords} records`],
        ["skipped", `${summary.skippedBytes} B`],
    ]);
}

function compareCard(compare: ComparePayload): HTMLElement {
    const rows: [string, string][] = [
        ["aligned", `${compare.alignedSamples} samples`],
        ["unmatched", String(compare.unmatched)],
        ["duration", formatDuration(compare.durationS)],
        ["max gap", `${(compare.maxGapUs / 1000).toFixed(0)} ms`],
    ];
    for (const metric of compare.metrics) {
        rows.push([
            metric.name,
            `rms ${metric.rms.toFixed(3)} ${metric.unit}, max ${metric.max.toFixed(3)} ${metric.unit}`,
        ]);
        const worst = metric.worstWindows[0];
        if (worst) {
            rows.push([`${metric.name} worst`, `${worst.rms.toFixed(3)} at ${worst.startS.toFixed(1)} s`]);
        }
    }
    return card("streams score (hub)", rows);
}

function downloadCard(name: string, entry: RecordingEntry | undefined): HTMLElement {
    const box = document.createElement("div");
    box.className = "card";
    const head = document.createElement("div");
    head.className = "card-title";
    head.textContent = "download";
    box.appendChild(head);

    const parts: [string, string | undefined][] =
        entry?.kind === "streams"
            ? [
                  ["telemetry csv", "telemetry"],
                  ...(entry.simRawFile ? ([["sim raw csv", "simraw"]] as [string, string][]) : []),
              ]
            : [
                  ["raw blackbox", "raw"],
                  ["csv", "csv"],
              ];
    for (const [label, part] of parts) {
        const link = document.createElement("a");
        link.className = "card-link";
        link.href = fileUrl(name, part);
        link.download = "";
        link.textContent = label;
        box.appendChild(link);
    }
    return box;
}
