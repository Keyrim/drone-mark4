/**
 * The hub REST API, as seen from a page. Same origin as the page itself, so
 * the paths are relative and there is no host to configure.
 *
 * Tables come back column-oriented in their header and row-oriented in their
 * data: a "columns" array names the fields, and every row is an array in
 * that order. The hub decides the stride, the page never asks for more
 * points than it can draw.
 */

/** One recording the hub holds, newest first in the listing. */
export interface RecordingEntry {
    name: string;
    kind: "blackbox" | "streams";
    sizeBytes: number;
    modifiedUnixS: number;
    /** blackbox only */
    estimatedRecords?: number;
    /** streams only */
    telemetryFile?: string;
    /** streams only, empty when the run had no simulator */
    simRawFile?: string;
}

export interface RecordingList {
    logDir: string;
    recordings: RecordingEntry[];
}

/** A strided window of one stream. */
export interface Table {
    total: number;
    stride: number;
    count: number;
    columns: string[];
    rows: number[][];
}

export interface RecordingPayload {
    name: string;
    kind: "blackbox" | "streams";
    /** streams only */
    window?: { fromUs: number; toUs: number };
    /** streams only */
    telemetry?: Table;
    /** streams only, absent when the run had no simulator */
    simRaw?: Table;
    /** blackbox only: the table is inlined in the payload */
    total?: number;
    stride?: number;
    count?: number;
    skippedBytes?: number;
    columns?: string[];
    rows?: number[][];
}

export interface CompareMetric {
    name: string;
    unit: string;
    rms: number;
    max: number;
    worstWindows: { startS: number; rms: number }[];
}

export interface ComparePayload {
    maxGapUs: number;
    alignedSamples: number;
    unmatched: number;
    durationS: number;
    metrics: CompareMetric[];
    series: Table;
}

export interface SummaryPayload {
    records: number;
    durationS: number;
    rateHz: number;
    accelNormG: { min: number; max: number };
    killRecords: number;
    skippedBytes: number;
}

export type Params = Record<string, string | number | undefined>;

function url(path: string, params: Params = {}): string {
    const query = new URLSearchParams();
    for (const [key, value] of Object.entries(params)) {
        if (value !== undefined && value !== "") {
            query.set(key, String(value));
        }
    }
    const text = query.toString();
    return text === "" ? path : `${path}?${text}`;
}

async function getJson<T>(path: string, params: Params = {}): Promise<T> {
    const response = await fetch(url(path, params));
    if (!response.ok) {
        throw new Error(`${path}: ${response.status} ${response.statusText}`);
    }
    return (await response.json()) as T;
}

export function listRecordings(): Promise<RecordingList> {
    return getJson<RecordingList>("/api/recordings");
}

export function getRecording(name: string, params: Params = {}): Promise<RecordingPayload> {
    return getJson<RecordingPayload>("/api/recording", { name, ...params });
}

export function getCompare(name: string, params: Params = {}): Promise<ComparePayload> {
    return getJson<ComparePayload>("/api/compare", { name, ...params });
}

export function getSummary(name: string): Promise<SummaryPayload> {
    return getJson<SummaryPayload>("/api/summary", { name });
}

/** Download link for a recording, or for one part of a streams pair. */
export function fileUrl(name: string, part?: string): string {
    return url("/api/file", { name, part });
}

/** Blackbox tables are inlined in the payload; normalize them into a Table. */
export function blackboxTable(payload: RecordingPayload): Table | null {
    if (!payload.columns || !payload.rows) {
        return null;
    }
    return {
        total: payload.total ?? payload.rows.length,
        stride: payload.stride ?? 1,
        count: payload.count ?? payload.rows.length,
        columns: payload.columns,
        rows: payload.rows,
    };
}

/** Index of every column, so a row is read by name and never by position. */
export function columnIndex(table: Table): Map<string, number> {
    return new Map(table.columns.map((name, i) => [name, i]));
}
