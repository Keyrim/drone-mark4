// The log side of the extension, pure: how a Log line is printed, and what
// the log levels view shows. The provider only turns these items into
// TreeItems, so the shapes below are what the tests drive.

import { LogLevel } from "./gen/mark4_pb";

import { hexNodeId, kindIcon } from "./model";

/** Level names in wire order, TRACE first: also the quick pick. */
export const LEVEL_NAMES = ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"] as const;

export function levelName(level: LogLevel): string {
    return LEVEL_NAMES[level] ?? `level ${level}`;
}

/** Pads to a width, cutting what is longer: the columns must stay columns. */
function column(text: string, width: number): string {
    return text.length > width ? text.slice(0, width) : text.padEnd(width);
}

const KIND_WIDTH = 8;
const MODULE_WIDTH = 24;

/**
 * One log line as the output channel prints it (the channel prepends its own
 * timestamp and level): kind, node id, module, then the text.
 */
export function formatLogLine(kind: string, nodeId: number, module: string, text: string): string {
    return `${column(kind, KIND_WIDTH)} | ${hexNodeId(nodeId)} | ${column(module, MODULE_WIDTH)} | ${text}`;
}

/** One (node, module) pair a level change applies to. */
export interface LevelTarget {
    readonly nodeId: number;
    readonly moduleId: number;
}

/** One item of the log levels tree, whatever the grouping. */
export interface LevelItem {
    readonly key: string;
    readonly label: string;
    readonly description: string;
    readonly icon: string;
    /** Every module a "Set level..." on this item would move. */
    readonly targets: LevelTarget[];
    readonly children: LevelItem[];
}

/** What the view needs of a node: its identity and its module table. */
export interface LevelNode {
    readonly id: number;
    readonly kind: number;
    readonly kindName: string;
    readonly name: string;
    readonly logModules: readonly { id: number; name: string; level: LogLevel }[];
}

export type LevelMode = "byNode" | "byModule";

/** The prefix of a module name, "" when the name has no `/`. */
function prefixOf(name: string): string {
    const slash = name.indexOf("/");
    return slash < 0 ? "" : name.slice(0, slash + 1);
}

/**
 * Groups items by the prefix of their module names. A prefix worn by a
 * single module stays flat: a folder holding one line is a click for
 * nothing.
 */
function foldPrefixes(keyPrefix: string, entries: { name: string; item: LevelItem }[]): LevelItem[] {
    const groups = new Map<string, { name: string; item: LevelItem }[]>();
    for (const entry of entries) {
        const prefix = prefixOf(entry.name);
        const list = groups.get(prefix);
        if (list) {
            list.push(entry);
        } else {
            groups.set(prefix, [entry]);
        }
    }
    const out: LevelItem[] = [];
    for (const [prefix, list] of groups) {
        if (prefix === "" || list.length < 2) {
            out.push(...list.map((entry) => entry.item));
            continue;
        }
        const children = list.map((entry) => ({ ...entry.item, label: entry.name.slice(prefix.length) }));
        out.push({
            key: `${keyPrefix}${prefix}`,
            label: prefix,
            description: `${children.length} modules`,
            icon: "folder",
            targets: children.flatMap((child) => child.targets),
            children,
        });
    }
    return out.sort((a, b) => a.label.localeCompare(b.label));
}

/** The level shown on a group of modules: the level, or "mixed". */
function levelSummary(levels: LogLevel[]): string {
    const first = levels[0];
    return first !== undefined && levels.every((level) => level === first) ? levelName(first) : "mixed";
}

/** The tree of the log levels view, by node or by module name. */
export function buildLevelTree(nodes: readonly LevelNode[], mode: LevelMode): LevelItem[] {
    const known = nodes.filter((node) => node.logModules.length > 0);
    if (mode === "byNode") {
        return known.map((node) => {
            const modules = [...node.logModules].sort((a, b) => a.name.localeCompare(b.name));
            const entries = modules.map((module) => ({
                name: module.name,
                item: {
                    key: `${hexNodeId(node.id)}:${module.id}`,
                    label: module.name,
                    description: levelName(module.level),
                    icon: "output",
                    targets: [{ nodeId: node.id, moduleId: module.id }],
                    children: [],
                },
            }));
            const children = foldPrefixes(`${hexNodeId(node.id)}:`, entries);
            return {
                key: `node:${hexNodeId(node.id)}`,
                label: node.name,
                description: `${node.kindName} ${hexNodeId(node.id)}`,
                icon: kindIcon(node.kind),
                targets: modules.map((module) => ({ nodeId: node.id, moduleId: module.id })),
                children,
            };
        });
    }
    // By module: one entry per module name, the nodes that have it below it.
    const byName = new Map<string, { node: LevelNode; module: { id: number; name: string; level: LogLevel } }[]>();
    for (const node of known) {
        for (const module of node.logModules) {
            const list = byName.get(module.name);
            if (list) {
                list.push({ node, module });
            } else {
                byName.set(module.name, [{ node, module }]);
            }
        }
    }
    const entries = [...byName.entries()]
        .sort(([a], [b]) => a.localeCompare(b))
        .map(([name, holders]) => ({
            name,
            item: {
                key: `module:${name}`,
                label: name,
                description: levelSummary(holders.map((holder) => holder.module.level)),
                icon: "output",
                targets: holders.map((holder) => ({ nodeId: holder.node.id, moduleId: holder.module.id })),
                children: holders.map((holder) => ({
                    key: `module:${name}:${hexNodeId(holder.node.id)}`,
                    label: holder.node.name,
                    description: `${holder.node.kindName} ${hexNodeId(holder.node.id)} ${levelName(holder.module.level)}`,
                    icon: kindIcon(holder.node.kind),
                    targets: [{ nodeId: holder.node.id, moduleId: holder.module.id }],
                    children: [],
                })),
            },
        }));
    return foldPrefixes("module:", entries);
}
