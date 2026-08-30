// The nodes view: one line per node of the gateway's table, whatever it is
// and whoever started it. Presence, identity and link health come from the
// wire; the start and stop actions only exist for what runs on this machine.

import * as vscode from "vscode";

import { type GatewayStatus, type NodeTable } from "./gen/gateway_pb";
import { NodeKind } from "./gen/mark4_pb";
import { type NodeRow, nodeRows, simInstance } from "./model";

export class NodeItem extends vscode.TreeItem {
    constructor(readonly row: NodeRow) {
        super(row.name);
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
    private readonly changed = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.changed.event;
    private rows: NodeRow[] = [];
    private table: NodeTable | undefined;
    private wireHash = 0;
    private online = false;

    setOnline(online: boolean): void {
        this.online = online;
        if (!online) {
            this.table = undefined;
            this.rows = [];
        }
        this.changed.fire();
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
        return this.rows.map((row) => new NodeItem(row));
    }

    private rebuild(): void {
        this.rows = this.table === undefined ? [] : nodeRows(this.table.nodes, this.wireHash);
        this.changed.fire();
    }
}
