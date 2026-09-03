/**
 * The stored shapes of the telemetry page: a view config (what is ticked,
 * how it is laid out, at what period, and no data) and the CSV export.
 * DOM-free and fetch-free: these turn a model into text and back, nothing
 * more.
 *
 * A config names its series by descriptor name, so it applies to any node
 * whatever ids that node hands out today.
 */

import { TelemetryUnit } from "../gen/mark4_pb";
import { type SeriesSpec, unitLabel } from "./model";

/** Version of the view config document. Bumped when the shape changes. */
export const CONFIG_VERSION = 1;

/** What is ticked and how it is laid out, with no data in it. */
export interface ViewConfig {
    version: number;
    periodMs: number;
    series: { name: string; unit: TelemetryUnit; color: string; laneId: number }[];
}

/** One series, ready to be exported. */
export interface SeriesData {
    readonly t: readonly number[];
    readonly v: readonly (number | null)[];
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
 * Parses a view config. Everything is checked, because the file may have
 * been hand-edited or come from an older page: a document that does not
 * hold up is refused whole rather than half applied.
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

/**
 * The name an export is stored under: the config it came from and the
 * instant, so two exports of one config never collide and a directory of
 * them reads in order. Only what the hub accepts as a name survives.
 *
 * @param configName the loaded config, "" when the selection was never named
 * @param at the instant, local time
 */
export function exportName(configName: string, at: Date): string {
    const base = configName.replace(/[^A-Za-z0-9_-]/g, "").slice(0, 40) || "telemetry";
    const two = (n: number): string => String(n).padStart(2, "0");
    const stamp =
        `${at.getFullYear()}${two(at.getMonth() + 1)}${two(at.getDate())}` +
        `-${two(at.getHours())}${two(at.getMinutes())}${two(at.getSeconds())}`;
    return `${base}-${stamp}`;
}
