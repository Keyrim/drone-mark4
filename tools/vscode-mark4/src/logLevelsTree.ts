// The log levels view: every module of every node and its threshold, grouped
// by node or by module name. A level is set by sending one LogControl per
// module under the item, then a query so the node republishes its table.

import * as vscode from "vscode";

import { type NodeTable } from "./gen/gateway_pb";
import { NodeKind } from "./gen/mark4_pb";
import { buildLevelTree, type LevelItem, type LevelMode, type LevelNode } from "./logTree";
import { kindName } from "./model";

export class LevelTreeItem extends vscode.TreeItem {
    constructor(readonly item: LevelItem) {
        super(
            item.label,
            item.children.length > 0
                ? vscode.TreeItemCollapsibleState.Collapsed
                : vscode.TreeItemCollapsibleState.None,
        );
        this.description = item.description;
        this.iconPath = new vscode.ThemeIcon(item.icon);
        this.contextValue = item.targets.length > 0 ? "logTarget" : "";
    }
}

/** Turns the gateway's table into what the pure tree builder needs. */
function toLevelNodes(table: NodeTable | undefined): LevelNode[] {
    return (table?.nodes ?? []).map((node) => {
        const kind = node.announce?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED;
        const word = kindName(kind);
        return {
            id: node.id,
            kind,
            kindName: word,
            name: node.announce?.name || word,
            logModules: node.logModules,
        };
    });
}

export class LogLevelsProvider implements vscode.TreeDataProvider<LevelTreeItem> {
    private readonly changed = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.changed.event;
    private nodes: LevelNode[] = [];
    private mode: LevelMode = "byNode";

    setTable(table: NodeTable | undefined): void {
        this.nodes = toLevelNodes(table);
        this.changed.fire();
    }

    /** Switches the grouping; the title action calls it. */
    toggleMode(): LevelMode {
        this.mode = this.mode === "byNode" ? "byModule" : "byNode";
        this.changed.fire();
        return this.mode;
    }

    getTreeItem(item: LevelTreeItem): vscode.TreeItem {
        return item;
    }

    getChildren(element?: LevelTreeItem): LevelTreeItem[] {
        const items = element ? element.item.children : buildLevelTree(this.nodes, this.mode);
        return items.map((item) => new LevelTreeItem(item));
    }
}
