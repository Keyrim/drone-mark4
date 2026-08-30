// The nodes view: one line per node of the gateway's table, whatever it is
// and whoever started it. Presence, identity and link health come from the
// wire; the start and stop actions only exist for what runs on this machine.
//
// The table comes once a second and almost never differs: the items are kept
// and refreshed in place, and the change event only fires for the lines that
// read differently (VS Code redraws, and dismisses tooltips, on every fire).

import * as vscode from "vscode";

import { type GatewayStatus, type NodeTable } from "./gen/gateway_pb";
import { NodeKind } from "./gen/mark4_pb";
import { diffNodeRows, type NodeRow, nodeRows, simInstance } from "./model";

export class NodeItem extends vscode.TreeItem {
    constructor(public row: NodeRow) {
        super(row.name);
        this.apply(row);
    }

    /** Takes a new row without becoming another item: the same object is
     * what the change event, and VS Code's own tree, know. */
    apply(row: NodeRow): void {
        this.row = row;
        this.label = row.name;
        this.description = `${row.kindName} ${row.hex}`;
        this.tooltip = row.tooltip;
        this.iconPath = row.mismatch
            ? new vscode.ThemeIcon("warning", new vscode.ThemeColor("problemsWarningIcon.foreground"))
            : new vscode.ThemeIcon(
                  row.icon,
                  new vscode.ThemeColor(row.live ? "testing.iconPassed" : "descriptionForeground"),
              );
        if (row.mismatch) {
            this.description += " WIRE MISMATCH";
        } else if (!row.live) {
            this.description += " fading";
        }
        this.contextValue = "node" + (stoppable(row) ? ":stop" : "");
    }
}

/** True when a line owns a local process the extension can terminate. */
function stoppable(row: NodeRow): boolean {
    return (
        row.kind === NodeKind.GATEWAY ||
        row.kind === NodeKind.PLANT ||
        (row.kind === NodeKind.DRONE_SIM && simInstance(row.id) !== undefined)
    );
}

function offlineItem(): vscode.TreeItem {
    const item = new vscode.TreeItem("gateway offline: start the hub");
    item.iconPath = new vscode.ThemeIcon("debug-disconnect");
    item.contextValue = "gatewayDown";
    return item;
}

export class NodesProvider implements vscode.TreeDataProvider<vscode.TreeItem> {
    private readonly changed = new vscode.EventEmitter<vscode.TreeItem | undefined>();
    readonly onDidChangeTreeData = this.changed.event;
    private rows: NodeRow[] = [];
    private items = new Map<number, NodeItem>();
    private table: NodeTable | undefined;
    private wireHash = 0;
    private online = false;

    setOnline(online: boolean): void {
        this.online = online;
        if (!online) {
            this.table = undefined;
            this.rows = [];
            this.items.clear();
        }
        this.changed.fire(undefined);
    }

    setTable(table: NodeTable): void {
        this.table = table;
        this.rebuild();
    }

    setStatus(status: GatewayStatus): void {
        if (status.wireHash === this.wireHash) {
            return;
        }
        this.wireHash = status.wireHash;
        this.rebuild();
    }

    getTreeItem(item: vscode.TreeItem): vscode.TreeItem {
        return item;
    }

    getChildren(element?: vscode.TreeItem): vscode.TreeItem[] {
        if (element) {
            return [];
        }
        if (!this.online) {
            return [offlineItem()];
        }
        return this.rows.map((row) => this.items.get(row.id) ?? new NodeItem(row));
    }

    private rebuild(): void {
        const next = this.table === undefined ? [] : nodeRows(this.table.nodes, this.wireHash);
        const changes = diffNodeRows(this.rows, next);
        this.rows = next;
        if (changes.structural) {
            this.items = new Map(next.map((row) => [row.id, new NodeItem(row)]));
            this.changed.fire(undefined);
            return;
        }
        // The lines are the same lines: refresh them all (the tooltip
        // follows on the next redraw) and only fire for what reads
        // differently.
        const dirty: NodeItem[] = [];
        for (const row of next) {
            const item = this.items.get(row.id);
            if (item === undefined) {
                continue;
            }
            item.apply(row);
            if (changes.changed.includes(row.id)) {
                dirty.push(item);
            }
        }
        for (const item of dirty) {
            this.changed.fire(item);
        }
    }
}
