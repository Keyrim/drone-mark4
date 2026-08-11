// The bench view: hub state, godot state, and the two page entries. The hub
// state is one HTTP ping; the URL is the only thing the extension knows
// about the hub.

import * as http from "node:http";
import * as vscode from "vscode";
import { findExecution, godotRunning, log } from "./tasks";

export const HUB_URL = "http://127.0.0.1:47810";

export function pingHub(timeoutMs = 1000): Promise<boolean> {
    return new Promise((resolve) => {
        const request = http.get(HUB_URL, (response) => {
            response.resume();
            resolve(true);
        });
        request.setTimeout(timeoutMs, () => request.destroy());
        request.on("error", () => resolve(false));
    });
}

export async function waitForHub(timeoutMs: number, token?: vscode.CancellationToken): Promise<boolean> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline && !token?.isCancellationRequested) {
        if (await pingHub()) {
            return true;
        }
        await new Promise((resolve) => setTimeout(resolve, 1000));
    }
    return false;
}

function pageItem(label: string, command: string, icon: string): vscode.TreeItem {
    const item = new vscode.TreeItem(label);
    item.iconPath = new vscode.ThemeIcon(icon);
    item.command = { command, title: label };
    return item;
}

export class BenchProvider implements vscode.TreeDataProvider<vscode.TreeItem> {
    private readonly changed = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.changed.event;
    private hubUp = false;
    private godotUp = false;

    constructor(context: vscode.ExtensionContext) {
        const poll = async (): Promise<void> => {
            const hub = await pingHub();
            const godot = await godotRunning();
            if (hub !== this.hubUp || godot !== this.godotUp) {
                log.info(`state change: hub ${hub ? "up" : "down"}, godot ${godot ? "up" : "down"}`);
                this.hubUp = hub;
                this.godotUp = godot;
                this.changed.fire();
            }
        };
        void poll();
        const timer = setInterval(() => void poll(), 3000);
        context.subscriptions.push(new vscode.Disposable(() => clearInterval(timer)));
        vscode.tasks.onDidStartTask(() => this.changed.fire(), undefined, context.subscriptions);
        vscode.tasks.onDidEndTask(() => this.changed.fire(), undefined, context.subscriptions);
    }

    getTreeItem(item: vscode.TreeItem): vscode.TreeItem {
        return item;
    }

    getChildren(element?: vscode.TreeItem): vscode.TreeItem[] {
        if (element) {
            return [];
        }
        return [
            this.hubItem(),
            this.godotItem(),
            pageItem("control page", "mark4.openControl", "browser"),
            pageItem("plots page", "mark4.openPlots", "graph-line"),
        ];
    }

    private hubItem(): vscode.TreeItem {
        const item = new vscode.TreeItem("hub");
        const execution = findExecution("run", "hub");
        item.description = this.hubUp ? HUB_URL : execution ? "starting" : "down";
        item.iconPath = this.hubUp
            ? new vscode.ThemeIcon("circle-filled", new vscode.ThemeColor("testing.iconPassed"))
            : new vscode.ThemeIcon("circle-outline");
        item.contextValue = execution ? "hubTask" : this.hubUp ? "hubExternal" : "hubDown";
        return item;
    }

    private godotItem(): vscode.TreeItem {
        const item = new vscode.TreeItem("godot sim");
        item.description = this.godotUp ? "running" : "";
        item.iconPath = this.godotUp
            ? new vscode.ThemeIcon("circle-filled", new vscode.ThemeColor("testing.iconPassed"))
            : new vscode.ThemeIcon("circle-outline");
        item.contextValue = this.godotUp ? "godotTask" : "godotIdle";
        return item;
    }
}
