// One tree item per apps.json entry. Build is always offered, run only when
// the entry declares a run command, debug only when the workspace has a
// launch configuration named "<app> ..." (the repo convention).

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";
import { findExecution } from "./tasks";

interface AppEntry {
    name: string;
    cmakePreset: string;
    target: string;
    run?: string[];
}

export class AppItem extends vscode.TreeItem {
    constructor(
        readonly app: AppEntry,
        readonly debugName: string | undefined,
        running: boolean,
    ) {
        super(app.name);
        this.description = `${app.cmakePreset} / ${app.target}` + (running ? " (running)" : "");
        this.iconPath = running
            ? new vscode.ThemeIcon("circle-filled", new vscode.ThemeColor("testing.iconPassed"))
            : new vscode.ThemeIcon("package");
        // A running app offers stop instead of run: a second instance would
        // only fight the first for its ports.
        this.contextValue =
            "app" + (running ? ":stop" : app.run ? ":run" : "") + (debugName ? ":debug" : "");
    }
}

export class AppsProvider implements vscode.TreeDataProvider<AppItem> {
    private readonly changed = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.changed.event;

    constructor(context: vscode.ExtensionContext) {
        const watcher = vscode.workspace.createFileSystemWatcher("**/apps.json");
        watcher.onDidChange(() => this.changed.fire(), undefined, context.subscriptions);
        watcher.onDidCreate(() => this.changed.fire(), undefined, context.subscriptions);
        watcher.onDidDelete(() => this.changed.fire(), undefined, context.subscriptions);
        context.subscriptions.push(watcher);
        vscode.tasks.onDidStartTask(() => this.changed.fire(), undefined, context.subscriptions);
        vscode.tasks.onDidEndTask(() => this.changed.fire(), undefined, context.subscriptions);
    }

    getTreeItem(item: AppItem): vscode.TreeItem {
        return item;
    }

    getChildren(element?: AppItem): AppItem[] {
        if (element) {
            return [];
        }
        const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        if (!root) {
            return [];
        }
        let apps: AppEntry[];
        try {
            apps = JSON.parse(fs.readFileSync(path.join(root, "software", "apps.json"), "utf-8")).apps;
        } catch {
            return [];
        }
        const launch =
            vscode.workspace.getConfiguration("launch").get<{ name: string }[]>("configurations") ?? [];
        return apps.map(
            (app) =>
                new AppItem(
                    app,
                    launch.find((config) => config.name.startsWith(app.name + " "))?.name,
                    findExecution("run", app.name) !== undefined,
                ),
        );
    }
}
