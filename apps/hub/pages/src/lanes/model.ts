/**
 * Lane viewer state: per-series sample buffers and the named view configs
 * that decide which series share a lane. No DOM access.
 *
 * There is no session concept here. A page is either following the live hub
 * stream or showing one recording the hub already holds; recording itself is
 * the hub's business, and the blackbox is the session.
 */

import type { SeriesDef } from "../shared/series";

/** Where the samples come from. */
export type Mode = "live" | "replay";

/** Points kept per series before the oldest ones are dropped. */
export const RING_CAPACITY = 12000;

/** Fraction of the ring released at once when it overflows. */
const TRIM_FRACTION = 0.1;

/**
 * Ascending sample buffer of one series, bounded like a ring: past the
 * capacity the oldest samples are dropped.
 */
export class SeriesBuffer {
    /** Seconds relative to the origin of the stream, ascending */
    readonly t: number[] = [];
    /** null marks a hole so the plot breaks the line instead of drawing a chord */
    readonly v: (number | null)[] = [];

    constructor(
        readonly def: SeriesDef,
        readonly capacity: number = RING_CAPACITY
    ) {}

    /**
     * Append one sample. Samples that do not move forward in time are
     * dropped: uPlot requires an ascending x, and a source restart or a
     * clock step would otherwise fold the plot onto itself.
     */
    push(t: number, value: number | null): boolean {
        const n = this.t.length;
        if (n > 0 && t <= (this.t[n - 1] as number)) {
            return false;
        }
        this.t.push(t);
        this.v.push(value);
        if (this.t.length > this.capacity) {
            // ponytail: amortized trim rather than a true circular buffer;
            // uPlot wants contiguous ascending arrays, and one splice every
            // 1200 samples is far cheaper than a copy on every frame.
            const drop = Math.ceil(this.capacity * TRIM_FRACTION);
            this.t.splice(0, drop);
            this.v.splice(0, drop);
        }
        return true;
    }

    clear(): void {
        this.t.length = 0;
        this.v.length = 0;
    }

    last(): number | null {
        const n = this.v.length;
        return n > 0 ? (this.v[n - 1] as number | null) : null;
    }

    endS(): number {
        const n = this.t.length;
        return n > 0 ? (this.t[n - 1] as number) : 0;
    }
}

/** One horizontal band: a title and the series keys it draws. */
export interface LaneConfig {
    title: string;
    keys: string[];
}

/** A named set of lanes. This is a view, not a recording. */
export interface ViewConfig {
    name: string;
    lanes: LaneConfig[];
}

/**
 * Group the buffers lane by lane, in config order. Keys with no buffer are
 * skipped and empty lanes disappear, so a config naming a series the current
 * stream cannot feed still opens.
 */
export function groupByLane(
    lanes: LaneConfig[],
    buffers: Map<string, SeriesBuffer>
): { config: LaneConfig; buffers: SeriesBuffer[] }[] {
    const out: { config: LaneConfig; buffers: SeriesBuffer[] }[] = [];
    for (const config of lanes) {
        const group: SeriesBuffer[] = [];
        for (const key of config.keys) {
            const buffer = buffers.get(key);
            if (buffer) {
                group.push(buffer);
            }
        }
        if (group.length > 0) {
            out.push({ config, buffers: group });
        }
    }
    return out;
}

const STORE_KEY = "mark4.pages.viewConfigs";

/** Named view configs saved in this browser. */
export function loadViewConfigs(): ViewConfig[] {
    try {
        const raw = localStorage.getItem(STORE_KEY);
        const parsed: unknown = raw === null ? [] : JSON.parse(raw);
        return Array.isArray(parsed) ? (parsed as ViewConfig[]) : [];
    } catch {
        return [];
    }
}

export function saveViewConfigs(configs: ViewConfig[]): void {
    try {
        localStorage.setItem(STORE_KEY, JSON.stringify(configs));
    } catch {
        // A browser refusing storage costs the saved views, nothing else
    }
}
