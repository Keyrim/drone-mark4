/**
 * The data model of the telemetry page. DOM-free and socket-free, so the
 * tests drive it directly.
 *
 * The identity of a series is the descriptor NAME, never the numeric id: an
 * id is an index into the node's frozen table and a reboot hands the same
 * number to another measure. The ids only ever appear in `routes`, rebuilt
 * from the node's current table, and in the enable the page sends.
 */

import { TelemetryUnit } from "../gen/mark4_pb";
import { SeriesBuffer, type SeriesDef } from "../lanes/model";
import { PALETTE } from "../shared/nodes";

/** Samples kept per series before the oldest ones are dropped. */
export const MAX_POINTS_PER_SERIES = 200000;

/** Fraction of a full buffer released at once: half, so a trim is rare. */
const TRIM_FRACTION = 0.5;

const US_PER_S = 1e6;

/** One measure a node exposes, as the gateway published it. */
export interface Descriptor {
    readonly id: number;
    readonly name: string;
    readonly unit: TelemetryUnit;
}

/** One series the page draws: what it is, how it looks, where it sits. */
export interface SeriesSpec {
    /** Descriptor name, the stable identity across reboots */
    readonly name: string;
    readonly unit: TelemetryUnit;
    readonly color: string;
    /** Lane the series is drawn in; several series may share one */
    laneId: number;
}

/** The axis label of a unit, empty for the unitless ones. */
const UNIT_LABELS: Record<number, string> = {
    [TelemetryUnit.UNITLESS]: "",
    [TelemetryUnit.M]: "m",
    [TelemetryUnit.M_PER_S]: "m/s",
    [TelemetryUnit.M_PER_S2]: "m/s2",
    [TelemetryUnit.RAD]: "rad",
    [TelemetryUnit.RAD_PER_S]: "rad/s",
    [TelemetryUnit.PA]: "Pa",
    [TelemetryUnit.CELSIUS]: "degC",
    [TelemetryUnit.V]: "V",
    [TelemetryUnit.A]: "A",
    [TelemetryUnit.US]: "us",
    [TelemetryUnit.COUNT]: "count",
};

export function unitLabel(unit: TelemetryUnit): string {
    return UNIT_LABELS[unit] ?? "";
}

/** Two series may share a lane only when they read in the same unit. */
export function sameUnit(a: SeriesSpec, b: SeriesSpec): boolean {
    return a.unit === b.unit;
}

/**
 * The hue a new selection takes: the first of the palette no series uses
 * yet, so two curves of one lane are told apart without asking.
 */
export function nextColor(used: readonly SeriesSpec[]): string {
    const taken = new Set(used.map((series) => series.color));
    return PALETTE.find((color) => !taken.has(color)) ?? (PALETTE[used.length % PALETTE.length] as string);
}

/** What a series looks like to the lane renderer. */
export function seriesDefOf(spec: SeriesSpec): SeriesDef {
    return {
        key: spec.name,
        label: spec.name,
        unit: unitLabel(spec.unit),
        color: spec.color,
    };
}

/** One sampling instant, as a TelemetryData message carries it. */
export interface Sample {
    readonly id: number;
    readonly value: number;
}

/**
 * The selected series, their buffers and the routing from the node's
 * current ids to them.
 */
export class TelemetryModel {
    private specs: SeriesSpec[] = [];
    private buffers = new Map<string, SeriesBuffer>();
    /** Descriptor id of the node's current table to the buffer it feeds */
    private routes = new Map<number, SeriesBuffer>();
    /** Series whose name the node's current table does not carry */
    private stale = new Set<string>();
    private t0: number | null = null;
    private lastT = -Infinity;
    private nextLane = 1;

    /** The selected series, in selection order. */
    list(): readonly SeriesSpec[] {
        return this.specs;
    }

    /** The buffer of one series, undefined when it is not selected. */
    buffer(name: string): SeriesBuffer | undefined {
        return this.buffers.get(name);
    }

    /** True when the node's current table does not carry that measure. */
    isStale(name: string): boolean {
        return this.stale.has(name);
    }

    has(name: string): boolean {
        return this.buffers.has(name);
    }

    /** Instant the first sample carried [us], null before any. */
    originUs(): number | null {
        return this.t0;
    }

    /** Seconds of data, 0 before the first sample. */
    durationS(): number {
        return this.lastT === -Infinity ? 0 : this.lastT;
    }

    /** Lane ids in use, ascending, with no gaps left by an empty lane. */
    lanes(): number[] {
        return [...new Set(this.specs.map((series) => series.laneId))].sort((a, b) => a - b);
    }

    /**
     * Selects one measure. A new selection starts in a lane of its own; the
     * color is the first unused hue.
     */
    add(descriptor: Descriptor): void {
        if (this.buffers.has(descriptor.name)) {
            return;
        }
        const spec: SeriesSpec = {
            name: descriptor.name,
            unit: descriptor.unit,
            color: nextColor(this.specs),
            laneId: this.nextLane,
        };
        this.nextLane += 1;
        this.specs.push(spec);
        this.buffers.set(spec.name, new SeriesBuffer(seriesDefOf(spec), MAX_POINTS_PER_SERIES, TRIM_FRACTION));
        this.routes.set(descriptor.id, this.buffers.get(spec.name) as SeriesBuffer);
    }

    /**
     * Selects a group of measures at once, the ones already selected left
     * where they are. The new ones are laid out one lane per unit: a group
     * is ticked to be compared, and a shared y axis is honest between
     * measures that read alike. A member already selected lends its lane to
     * the newcomers of its unit.
     */
    addGroup(descriptors: readonly Descriptor[]): void {
        const laneByUnit = new Map<TelemetryUnit, number>();
        for (const descriptor of descriptors) {
            const spec = this.specs.find((series) => series.name === descriptor.name);
            if (spec !== undefined && !laneByUnit.has(spec.unit)) {
                laneByUnit.set(spec.unit, spec.laneId);
            }
        }
        for (const descriptor of descriptors) {
            if (this.buffers.has(descriptor.name)) {
                continue;
            }
            this.add(descriptor);
            const lane = laneByUnit.get(descriptor.unit);
            if (lane === undefined) {
                laneByUnit.set(descriptor.unit, this.specs[this.specs.length - 1]?.laneId ?? 0);
            } else {
                this.setLane(descriptor.name, lane);
            }
        }
    }

    remove(name: string): void {
        this.specs = this.specs.filter((series) => series.name !== name);
        const buffer = this.buffers.get(name);
        this.buffers.delete(name);
        this.stale.delete(name);
        for (const [id, routed] of [...this.routes]) {
            if (routed === buffer) {
                this.routes.delete(id);
            }
        }
    }

    /** Moves one series to a lane; -1 asks for a lane of its own. */
    setLane(name: string, laneId: number): void {
        const spec = this.specs.find((series) => series.name === name);
        if (spec === undefined) {
            return;
        }
        if (laneId < 0) {
            spec.laneId = this.nextLane;
            this.nextLane += 1;
            return;
        }
        spec.laneId = laneId;
    }

    /** Reorders the lanes so that `lanes` is the order they are drawn in. */
    orderLanes(order: readonly number[]): void {
        const rank = new Map(order.map((laneId, index) => [laneId, index]));
        for (const spec of this.specs) {
            const at = rank.get(spec.laneId);
            if (at !== undefined) {
                // The rank becomes the lane id, offset so it never collides
                // with a lane still holding its old number mid-renumbering.
                spec.laneId = at + this.nextLane;
            }
        }
        this.compactLanes();
    }

    /**
     * Rebinds the series to a node's table, by name. Ids that are not in the
     * table any more stop feeding anything and their series is flagged
     * stale; a series whose name is back is bound again to whatever id the
     * table now gives it.
     */
    bind(table: readonly Descriptor[]): void {
        this.routes.clear();
        this.stale.clear();
        const byName = new Map(table.map((descriptor) => [descriptor.name, descriptor]));
        for (const spec of this.specs) {
            const descriptor = byName.get(spec.name);
            const buffer = this.buffers.get(spec.name);
            if (descriptor === undefined || buffer === undefined) {
                this.stale.add(spec.name);
                continue;
            }
            this.routes.set(descriptor.id, buffer);
        }
    }

    /** The ids to enable on the node, for the series that are bound. */
    enabledIds(): number[] {
        return [...this.routes.keys()].sort((a, b) => a - b);
    }

    /**
     * Takes one sampling instant. A timestamp that does not strictly
     * increase is dropped whole: the buffers are ascending by contract, and
     * a rebased stream is a new recording, never an inferred one.
     *
     * @returns true when the samples were kept
     */
    ingest(timestampUs: number, samples: readonly Sample[]): boolean {
        if (this.t0 === null) {
            this.t0 = timestampUs;
        }
        const t = (timestampUs - this.t0) / US_PER_S;
        if (t <= this.lastT) {
            return false;
        }
        this.lastT = t;
        for (const sample of samples) {
            this.routes.get(sample.id)?.push(t, sample.value);
        }
        return true;
    }

    /**
     * Breaks every curve here: an explicit hole, so a lane draws a gap
     * instead of a chord across the silence. The marker sits a hair past
     * the last sample, which keeps the buffers strictly ascending.
     */
    markGap(): void {
        if (this.lastT === -Infinity) {
            return;
        }
        const t = this.lastT + Number.EPSILON * Math.max(1, Math.abs(this.lastT));
        if (t <= this.lastT) {
            return;
        }
        this.lastT = t;
        for (const buffer of this.buffers.values()) {
            buffer.push(t, null);
        }
    }

    /** Drops every sample, keeping the selection. */
    clearData(): void {
        for (const buffer of this.buffers.values()) {
            buffer.clear();
        }
        this.t0 = null;
        this.lastT = -Infinity;
    }

    /** Drops the selection and the data with it. */
    reset(): void {
        this.specs = [];
        this.buffers.clear();
        this.routes.clear();
        this.stale.clear();
        this.t0 = null;
        this.lastT = -Infinity;
        this.nextLane = 1;
    }

    /** Renumbers the lanes 0..n-1 in their current order, gaps removed. */
    private compactLanes(): void {
        const order = [...new Set(this.specs.map((series) => series.laneId))].sort((a, b) => a - b);
        const rank = new Map(order.map((laneId, index) => [laneId, index]));
        for (const spec of this.specs) {
            spec.laneId = rank.get(spec.laneId) ?? 0;
        }
        this.nextLane = order.length;
    }
}
