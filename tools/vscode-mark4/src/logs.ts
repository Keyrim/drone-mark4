// The "Mark4 Logs" output channel: what was received (the store) redrawn
// through what is displayed (the filter and the names). The channel is a
// plain one, so the extension owns the whole line, its timestamp and its
// filtering; VS Code neither stamps nor hides anything.
//
// The gateway's own lines arrive from its node id like everyone else's, so
// nothing here knows about the hub. Only the state of the link itself has
// no node: it is stored under a pseudo node, id 0, named "gateway link", so
// a redraw keeps it in place like any other line.

import * as vscode from "vscode";

import { type NodeTable } from "./gen/gateway_pb";
import { type Log, LogLevel, type LogModuleInfo, NodeKind } from "./gen/mark4_pb";
import { type LogRecord, LogStore } from "./logStore";
import { type LevelTarget } from "./logTree";
import { LogFilter, type NameTable, type NodeNames, renderLogs, sameNames, visibleLine } from "./logView";
import { kindName } from "./model";

/** Enough for a burst not to redraw the whole projection line by line. */
const FLUSH_MS = 200;

/** The link itself, the one line that comes from no node. */
const LINK_NODE_ID = 0;
const LINK_NAMES: NodeNames = { kind: "gateway", modules: new Map([[0, "gateway link"]]) };

function namesOf(table: NodeTable, modules: ReadonlyMap<number, readonly LogModuleInfo[]>): NameTable {
    const names = new Map<number, NodeNames>([[LINK_NODE_ID, LINK_NAMES]]);
    for (const node of table.nodes) {
        names.set(node.id, {
            kind: kindName(node.announce?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED),
            modules: new Map((modules.get(node.id) ?? []).map((module) => [module.id, module.name])),
        });
    }
    return names;
}

export class LogChannel {
    // The channel is a plain one, given the language of the columns it
    // writes: the grammar of syntaxes/ colors them, nothing else is added.
    private readonly channel = vscode.window.createOutputChannel("Mark4 Logs", "mark4-log");
    private readonly store = new LogStore();
    private readonly filter = new LogFilter();
    private names: NameTable = new Map([[LINK_NODE_ID, LINK_NAMES]]);
    /** The modules of every node, as the gateway published them. */
    private modules = new Map<number, readonly LogModuleInfo[]>();
    private table: NodeTable | undefined;
    private search = "";
    /** Records received since the last flush, still to append. */
    private pending: LogRecord[] = [];
    private redraw = false;
    private timer: ReturnType<typeof setTimeout> | undefined;

    dispose(): void {
        if (this.timer !== undefined) {
            clearTimeout(this.timer);
        }
        this.channel.dispose();
    }

    /**
     * The table names the kinds and the modules. Names resolve at render
     * time, so a table arriving late names the lines that came before it:
     * whenever it changes, the whole projection is drawn again.
     */
    setTable(table: NodeTable): void {
        this.table = table;
        this.resolveNames();
    }

    /** One node's module table: the names its lines are rendered with. */
    setModules(node: number, modules: readonly LogModuleInfo[]): void {
        this.modules.set(node, modules);
        this.resolveNames();
    }

    /** One line of its own when the link came back: the gap is the news. */
    noteReconnect(): void {
        this.append({
            receivedAt: new Date(),
            nodeId: LINK_NODE_ID,
            moduleId: 0,
            level: LogLevel.INFO,
            text: "reconnected to the gateway",
        });
    }

    /** Stores the lines of one node: one as it arrives, or a whole ring. */
    write(src: number, lines: readonly Log[]): void {
        for (const line of lines) {
            this.append({
                receivedAt: new Date(),
                nodeId: src,
                moduleId: line.moduleId,
                level: line.level,
                text: line.text,
            });
        }
    }

    /** The display side of a "Set level...": the same scope, shown at once. */
    setLevel(targets: readonly LevelTarget[], level: LogLevel): void {
        this.filter.setLevel(targets, level);
        this.scheduleRedraw();
    }

    /** Hides or shows every line of a node; returns the new state. */
    toggleHidden(nodeId: number): boolean {
        const hidden = this.filter.toggleHidden(nodeId);
        this.scheduleRedraw();
        return hidden;
    }

    /** What the search box opens on. */
    currentSearch(): string {
        return this.search;
    }

    setSearch(text: string): void {
        this.search = text;
        this.scheduleRedraw();
    }

    /** Empties what was received; the display filter is left alone. */
    clear(): void {
        this.store.clear();
        this.pending = [];
        this.scheduleRedraw();
    }

    show(): void {
        this.channel.show(true);
    }

    /** The names come from the table and the module tables together. */
    private resolveNames(): void {
        if (this.table === undefined) {
            return;
        }
        const names = namesOf(this.table, this.modules);
        if (sameNames(this.names, names)) {
            return;
        }
        this.names = names;
        this.scheduleRedraw();
    }

    private append(record: LogRecord): void {
        this.store.push(record);
        this.pending.push(record);
        this.schedule();
    }

    private scheduleRedraw(): void {
        this.redraw = true;
        this.schedule();
    }

    private schedule(): void {
        if (this.timer === undefined) {
            this.timer = setTimeout(() => this.flush(), FLUSH_MS);
        }
    }

    private flush(): void {
        this.timer = undefined;
        if (this.redraw) {
            this.redraw = false;
            this.pending = [];
            this.channel.replace(renderLogs(this.store.records(), this.names, this.filter, this.search));
            return;
        }
        const records = this.pending;
        this.pending = [];
        for (const record of records) {
            const line = visibleLine(record, this.names, this.filter, this.search);
            if (line !== undefined) {
                this.channel.appendLine(line);
            }
        }
    }
}
