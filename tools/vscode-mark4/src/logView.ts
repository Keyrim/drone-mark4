// What is displayed of what was received: the projection of the store into
// the text of the output channel. Pure, so the tests drive it directly.
//
// The extension owns the whole line, timestamp included: the channel is a
// plain one and prints exactly what it is given. Names are resolved at
// render time, never at ingest, so a node table arriving late names the
// lines that came before it.

import { LogLevel } from "./gen/mark4_pb";
import { type LogRecord } from "./logStore";
import { type LevelTarget, levelName } from "./logTree";
import { hexNodeId } from "./model";

/** What a node's lines are named after: its kind and its module table. */
export interface NodeNames {
    readonly kind: string;
    readonly modules: ReadonlyMap<number, string>;
}

export type NameTable = ReadonlyMap<number, NodeNames>;

/** The level a node or a module is shown at until something says otherwise. */
export const DEFAULT_DISPLAY_LEVEL = LogLevel.INFO;

const LEVEL_WIDTH = 5;
const KIND_WIDTH = 9;
const MODULE_WIDTH = 24;

/** Pads to a width; only the module name is cut, the columns must hold. */
function column(text: string, width: number): string {
    return text.length > width ? text.slice(0, width) : text.padEnd(width);
}

/** The extension's clock, to the millisecond: HH:MM:SS.mmm. */
export function formatTime(at: Date): string {
    const two = (value: number): string => value.toString().padStart(2, "0");
    return (
        `${two(at.getHours())}:${two(at.getMinutes())}:${two(at.getSeconds())}` +
        `.${at.getMilliseconds().toString().padStart(3, "0")}`
    );
}

/** The key of one (node, module) pair in the display filter. */
function pairKey(nodeId: number, moduleId: number): string {
    return `${hexNodeId(nodeId)}:${moduleId}`;
}

/**
 * One log line, fixed columns, two spaces between them: time, level, kind,
 * node id, module, text.
 */
export function formatLogLine(
    at: Date,
    level: LogLevel,
    kind: string,
    nodeId: number,
    module: string,
    text: string,
): string {
    return [
        formatTime(at),
        levelName(level).padEnd(LEVEL_WIDTH),
        kind.padEnd(KIND_WIDTH),
        hexNodeId(nodeId),
        column(module, MODULE_WIDTH),
        text,
    ].join("  ");
}

/** The kind and the module name of a record, as far as the table knows. */
export function resolveNames(names: NameTable, record: LogRecord): { kind: string; module: string } {
    const known = names.get(record.nodeId);
    return {
        kind: known?.kind ?? "unknown",
        module: known?.modules.get(record.moduleId) ?? `#${record.moduleId}`,
    };
}

/** True when two name tables would render every line the same way. */
export function sameNames(left: NameTable, right: NameTable): boolean {
    if (left.size !== right.size) {
        return false;
    }
    for (const [id, names] of left) {
        const other = right.get(id);
        if (other === undefined || other.kind !== names.kind || other.modules.size !== names.modules.size) {
            return false;
        }
        for (const [moduleId, name] of names.modules) {
            if (other.modules.get(moduleId) !== name) {
                return false;
            }
        }
    }
    return true;
}

/**
 * What the channel shows of what the store holds: a minimum level per
 * (node, module), a hidden flag per node. Lowering a level hides stored
 * lines at once; raising it only shows what arrives from then on.
 */
export class LogFilter {
    private readonly levels = new Map<string, LogLevel>();
    private readonly hidden = new Set<number>();

    /** The same scope a "Set level..." sends to the nodes. */
    setLevel(targets: readonly LevelTarget[], level: LogLevel): void {
        for (const target of targets) {
            this.levels.set(pairKey(target.nodeId, target.moduleId), level);
        }
    }

    /** Hides or shows every line of a node; returns the new state. */
    toggleHidden(nodeId: number): boolean {
        if (this.hidden.delete(nodeId)) {
            return false;
        }
        this.hidden.add(nodeId);
        return true;
    }

    isHidden(nodeId: number): boolean {
        return this.hidden.has(nodeId);
    }

    allows(record: LogRecord): boolean {
        if (this.hidden.has(record.nodeId)) {
            return false;
        }
        return record.level >= (this.levels.get(pairKey(record.nodeId, record.moduleId)) ?? DEFAULT_DISPLAY_LEVEL);
    }
}

/**
 * The line of a record, or undefined when the filter or the search drops it.
 * The search runs on the whole line: a kind, a node id or a module name is
 * searched like the text.
 */
export function visibleLine(
    record: LogRecord,
    names: NameTable,
    filter: LogFilter,
    search: string,
): string | undefined {
    if (!filter.allows(record)) {
        return undefined;
    }
    const resolved = resolveNames(names, record);
    const line = formatLogLine(
        record.receivedAt,
        record.level,
        resolved.kind,
        record.nodeId,
        resolved.module,
        record.text,
    );
    return search === "" || line.toLowerCase().includes(search.toLowerCase()) ? line : undefined;
}

/** The whole projection, the text the channel is redrawn with. */
export function renderLogs(
    records: readonly LogRecord[],
    names: NameTable,
    filter: LogFilter,
    search = "",
): string {
    const lines: string[] = [];
    for (const record of records) {
        const line = visibleLine(record, names, filter, search);
        if (line !== undefined) {
            lines.push(line);
        }
    }
    return lines.join("\n");
}
