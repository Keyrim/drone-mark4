/**
 * The hub's telemetry store, seen from the page: the HTTP side of the hub
 * (software/hub/README.md, "HTTP API"). Recordings, their CSV exports and
 * the named view configs are files on the hub's disk, so a session survives
 * the tab that recorded it and can be opened from another machine.
 *
 * Every call answers a result rather than throwing: a hub that is down or a
 * refused name is a line in the toolbar, never an unhandled rejection.
 */

/** One stored file, as a listing describes it. */
export interface StoredEntry {
    readonly name: string;
    readonly bytes: number;
    /** Last write, unix seconds */
    readonly modified: number;
}

/** What a call came back with. */
export interface StoreResult<T> {
    readonly ok: boolean;
    readonly value?: T;
    readonly error?: string;
}

const SESSIONS = "/api/telemetry/sessions";
const EXPORTS = "/api/telemetry/exports";
const CONFIGS = "/api/telemetry/configs";

/** @returns the message the hub gave, or a generic one for that status */
async function errorOf(response: Response): Promise<string> {
    try {
        const body: unknown = await response.json();
        if (typeof body === "object" && body !== null && "error" in body) {
            const reason = (body as { error: unknown }).error;
            if (typeof reason === "string") {
                return reason;
            }
        }
    } catch {
        // A body that is not the JSON the hub normally answers with
    }
    return `HTTP ${response.status}`;
}

/** @returns the parsed listing of one collection */
async function list(path: string): Promise<StoreResult<StoredEntry[]>> {
    try {
        const response = await fetch(path);
        if (!response.ok) {
            return { ok: false, error: await errorOf(response) };
        }
        const body: unknown = await response.json();
        if (!Array.isArray(body)) {
            return { ok: false, error: "the hub answered something else than a listing" };
        }
        return { ok: true, value: body as StoredEntry[] };
    } catch (failure) {
        return { ok: false, error: String(failure) };
    }
}

/** @returns the body of one stored file, as text */
async function read(path: string): Promise<StoreResult<string>> {
    try {
        const response = await fetch(path);
        if (!response.ok) {
            return { ok: false, error: await errorOf(response) };
        }
        return { ok: true, value: await response.text() };
    } catch (failure) {
        return { ok: false, error: String(failure) };
    }
}

/** @returns whether the write went through */
async function write(path: string, body: string, type: string): Promise<StoreResult<number>> {
    try {
        const response = await fetch(path, {
            method: "PUT",
            headers: { "Content-Type": type },
            body,
        });
        if (!response.ok) {
            return { ok: false, error: await errorOf(response) };
        }
        return { ok: true, value: body.length };
    } catch (failure) {
        return { ok: false, error: String(failure) };
    }
}

/** @returns whether the file is gone */
async function remove(path: string): Promise<StoreResult<void>> {
    try {
        const response = await fetch(path, { method: "DELETE" });
        return response.ok ? { ok: true } : { ok: false, error: await errorOf(response) };
    } catch (failure) {
        return { ok: false, error: String(failure) };
    }
}

export const store = {
    listSessions: (): Promise<StoreResult<StoredEntry[]>> => list(SESSIONS),
    readSession: (name: string): Promise<StoreResult<string>> => read(`${SESSIONS}/${name}`),
    writeSession: (name: string, body: string): Promise<StoreResult<number>> =>
        write(`${SESSIONS}/${name}`, body, "application/json"),
    deleteSession: (name: string): Promise<StoreResult<void>> => remove(`${SESSIONS}/${name}`),

    /** The path a GET serves the export from, for the download link. */
    exportPath: (name: string): string => `${EXPORTS}/${name}.csv`,
    writeExport: (name: string, csv: string): Promise<StoreResult<number>> =>
        write(`${EXPORTS}/${name}.csv`, csv, "text/csv"),

    listConfigs: (): Promise<StoreResult<StoredEntry[]>> => list(CONFIGS),
    readConfig: (name: string): Promise<StoreResult<string>> => read(`${CONFIGS}/${name}`),
    writeConfig: (name: string, body: string): Promise<StoreResult<number>> =>
        write(`${CONFIGS}/${name}`, body, "application/json"),
    deleteConfig: (name: string): Promise<StoreResult<void>> => remove(`${CONFIGS}/${name}`),
};

/** Versioned key the working config is auto-saved under, per browser. */
export const WORKING_CONFIG_KEY = "mark4.pages.telemetry.v1";

/** Reads the working config back, null when there is none to read. */
export function loadWorkingConfig(): string | null {
    try {
        return localStorage.getItem(WORKING_CONFIG_KEY);
    } catch {
        return null;
    }
}

/** Auto-saves the working config. A browser refusing storage costs nothing. */
export function saveWorkingConfig(body: string): void {
    try {
        localStorage.setItem(WORKING_CONFIG_KEY, body);
    } catch {
        // A private window or a browser with site data blocked: the config
        // is still on the hub if it was ever saved under a name.
    }
}
