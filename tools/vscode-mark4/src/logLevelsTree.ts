// The log levels view: every module of every node and its threshold, grouped
// by node or by module name. A level is set by sending one LogControl per
// module under the item, then a query so the node republishes its table.
//
// The table comes once a second and the levels almost never move: the root
// items are kept and refreshed in place, and the change event only fires for
// the subtrees that read differently.

import * as vscode from "vscode";

import { type NodeTable } from "./gen/gateway_pb";
import { NodeKind } from "./gen/mark4_pb";
import { buildLevelTree, diffLevelTree, type LevelItem, type LevelMode, type LevelNode } from "./logTree";
import { kindName } from "./model";

export class LevelTreeItem extends vscode.TreeItem {
    constructor(public item: LevelItem) {
        super(item.label);
        this.apply(item);
    }

    /** Takes a new item without becoming another one: the same object is
     * what the change event, and VS Code's own tree, know. */
    apply(item: LevelItem): void {
        this.item = item;
        this.label = item.label;
        this.description = item.description;
        this.iconPath = new vscode.ThemeIcon(item.icon);
        this.collapsibleState =
            item.children.length > 0
                ? vscode.TreeItemCollapsibleState.Collapsed
                : vscode.TreeItemCollapsibleState.None;
        this.contextValue =
            item.targets.length === 0 ? "" : item.nodeId === undefined ? "logTarget" : "logTarget:node";
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
    private readonly changed = new vscode.EventEmitter<LevelTreeItem | undefined>();
    readonly onDidChangeTreeData = this.changed.event;
    private nodes: LevelNode[] = [];
    private roots: LevelItem[] = [];
    private items = new Map<string, LevelTreeItem>();
    private mode: LevelMode = "byNode";

    setTable(table: NodeTable | undefined): void {
        this.nodes = toLevelNodes(table);
        this.rebuild();
    }

    /** The nodes the view stands on, for a level change over all of them. */
    levelNodes(): readonly LevelNode[] {
        return this.nodes;
    }

    /** Switches the grouping; the title action calls it. */
    toggleMode(): LevelMode {
        this.mode = this.mode === "byNode" ? "byModule" : "byNode";
        this.rebuild();
        return this.mode;
    }

    getTreeItem(item: LevelTreeItem): vscode.TreeItem {
        return item;
    }

    getChildren(element?: LevelTreeItem): LevelTreeItem[] {
        if (element) {
            return element.item.children.map((child) => new LevelTreeItem(child));
        }
        return this.roots.map((root) => this.items.get(root.key) ?? new LevelTreeItem(root));
    }

    private rebuild(): void {
        const next = buildLevelTree(this.nodes, this.mode);
        const changes = diffLevelTree(this.roots, next);
        this.roots = next;
        if (changes.structural) {
            this.items = new Map(next.map((item) => [item.key, new LevelTreeItem(item)]));
            this.changed.fire(undefined);
            return;
        }
        for (const item of next) {
            this.items.get(item.key)?.apply(item);
        }
        for (const key of changes.changed) {
            const item = this.items.get(key);
            if (item !== undefined) {
                this.changed.fire(item);
            }
        }
    }
}
