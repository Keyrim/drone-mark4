/**
 * The stored shapes of the telemetry page: a session (a whole recording), a
 * view config (what is ticked, no data) and the CSV export. DOM-free and
 * fetch-free: these turn a model into text and back, nothing more.
 *
 * A stored series is named by its descriptor name, so a session opens
 * against any node - or none at all - whatever ids that node hands out
 * today.
 */

import { TelemetryUnit } from "../gen/mark4_pb";
import { type SeriesSpec, unitLabel } from "./model";

/** Version of the session document. Bumped when the shape changes. */
export const SESSION_VERSION = 1;

/** Version of the view config document. */
export const CONFIG_VERSION = 1;

/** The node a session was recorded from, as the page knew it. */
export interface SessionNode {
    id: number;
    kind: string;
    label: string;
}

/** One series of a stored session: what it is, and its samples. */
export interface SessionSeries {
    name: string;
    unit: TelemetryUnit;
    color: string;
    laneId: number;
    /** Seconds from t0Us, ascending */
    t: number[];
    /** null marks a hole: the lane breaks instead of drawing a chord */
    v: (number | null)[];
}

/** A whole recording, as `PUT /api/telemetry/sessions/<name>` stores it. */
export interface Session {
    version: number;
    name: string;
    node: SessionNode;
    /** Instant the first sample carried [us]: t = 0 of every series */
    t0Us: number;
    durationS: number;
    periodMs: number;
    series: SessionSeries[];
}

/** What is ticked and how it is laid out, with no data in it. */
export interface ViewConfig {
    version: number;
    periodMs: number;
    series: { name: string; unit: TelemetryUnit; color: string; laneId: number }[];
}

/** One series, ready to be stored. */
export interface SeriesData {
    readonly t: readonly number[];
    readonly v: readonly (number | null)[];
}

/**
 * Builds the session document. The samples are handed in rather than read
 * off the model, so the caller decides what a session covers.
 */
export function buildSession(
    name: string,
    node: SessionNode,
    t0Us: number,
    durationS: number,
    periodMs: number,
    specs: readonly SeriesSpec[],
    data: readonly SeriesData[]
): Session {
    return {
        version: SESSION_VERSION,
        name,
        node,
        t0Us,
        durationS,
        periodMs,
        series: specs.map((spec, index) => ({
            name: spec.name,
            unit: spec.unit,
            color: spec.color,
            laneId: spec.laneId,
            t: [...(data[index]?.t ?? [])],
            v: [...(data[index]?.v ?? [])],
        })),
    };
}

/** True when a value is a finite number or the null of a gap marker. */
function isSample(value: unknown): value is number | null {
    return value === null || (typeof value === "number" && Number.isFinite(value));
}

/**
 * Parses a stored session. Everything is checked, because the file may have
 * been hand-edited or come from an older page: a document that does not
 * hold up is refused whole rather than half loaded.
 *
 * @returns the session, or null when the text is not one
 */
export function parseSession(text: string): Session | null {
    let root: unknown = null;
    try {
        root = JSON.parse(text);
    } catch {
        return null;
    }
    if (typeof root !== "object" || root === null) {
        return null;
    }
    const raw = root as Record<string, unknown>;
    if (raw["version"] !== SESSION_VERSION || !Array.isArray(raw["series"])) {
        return null;
    }
    const node = (raw["node"] ?? {}) as Record<string, unknown>;
    const series: SessionSeries[] = [];
    for (const entry of raw["series"] as unknown[]) {
        if (typeof entry !== "object" || entry === null) {
            return null;
        }
        const item = entry as Record<string, unknown>;
        const t = item["t"];
        const v = item["v"];
        if (typeof item["name"] !== "string" || !Array.isArray(t) || !Array.isArray(v)) {
            return null;
        }
        if (t.length !== v.length || !t.every((x) => typeof x === "number" && Number.isFinite(x))) {
            return null;
        }
        if (!v.every(isSample)) {
            return null;
        }
        series.push({
            name: item["name"],
            unit: numberOr(item["unit"], TelemetryUnit.UNITLESS) as TelemetryUnit,
            color: typeof item["color"] === "string" ? item["color"] : "#3987e5",
            laneId: numberOr(item["laneId"], 0),
            t: t as number[],
            v: v as (number | null)[],
        });
    }
    return {
        version: SESSION_VERSION,
        name: typeof raw["name"] === "string" ? raw["name"] : "",
        node: {
            id: numberOr(node["id"], 0),
            kind: typeof node["kind"] === "string" ? node["kind"] : "",
            label: typeof node["label"] === "string" ? node["label"] : "",
        },
        t0Us: numberOr(raw["t0Us"], 0),
        durationS: numberOr(raw["durationS"], 0),
        periodMs: numberOr(raw["periodMs"], 0),
        series,
    };
}

/** @returns the value when it is a finite number, the fallback otherwise */
function numberOr(value: unknown, fallback: number): number {
    return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

/** What is ticked right now, ready to be stored or auto-saved. */
export function buildConfig(periodMs: number, specs: readonly SeriesSpec[]): ViewConfig {
    return {
        version: CONFIG_VERSION,
        periodMs,
        series: specs.map((spec) => ({
            name: spec.name,
            unit: spec.unit,
            color: spec.color,
            laneId: spec.laneId,
        })),
    };
}

/**
 * Parses a view config. Same stance as a session: refused whole rather than
 * half applied.
 *
 * @returns the config, or null when the text is not one
 */
export function parseConfig(text: string): ViewConfig | null {
    let root: unknown = null;
    try {
        root = JSON.parse(text);
    } catch {
        return null;
    }
    if (typeof root !== "object" || root === null) {
        return null;
    }
    const raw = root as Record<string, unknown>;
    if (raw["version"] !== CONFIG_VERSION || !Array.isArray(raw["series"])) {
        return null;
    }
    const series: ViewConfig["series"] = [];
    for (const entry of raw["series"] as unknown[]) {
        if (typeof entry !== "object" || entry === null) {
            return null;
        }
        const item = entry as Record<string, unknown>;
        if (typeof item["name"] !== "string") {
            return null;
        }
        series.push({
            name: item["name"],
            unit: numberOr(item["unit"], TelemetryUnit.UNITLESS) as TelemetryUnit,
            color: typeof item["color"] === "string" ? item["color"] : "#3987e5",
            laneId: numberOr(item["laneId"], 0),
        });
    }
    return { version: CONFIG_VERSION, periodMs: numberOr(raw["periodMs"], 50), series };
}

/** Quotes one CSV field, only when it needs it. */
function csvField(text: string): string {
    return /[",\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

/**
 * The CSV export, long format: one row per sample, so a spreadsheet or a
 * dataframe groups by series instead of guessing a common time base the
 * measures never shared. Gap markers carry no value and are skipped.
 */
export function toCsv(specs: readonly SeriesSpec[], data: readonly SeriesData[]): string {
    const rows: string[] = ["series,unit,t_s,value"];
    specs.forEach((spec, index) => {
        const samples = data[index];
        if (samples === undefined) {
            return;
        }
        const unit = csvField(unitLabel(spec.unit));
        const name = csvField(spec.name);
        samples.t.forEach((t, at) => {
            const value = samples.v[at];
            if (value === null || value === undefined) {
                return;
            }
            rows.push(`${name},${unit},${t},${value}`);
        });
    });
    return rows.join("\n") + "\n";
}
