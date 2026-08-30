// The bench view: what is not a node. Presence and start/stop of the local
// processes belong to the nodes view, which reads the gateway; what is left
// here is the hub URL (an HTTP ping, used by the bench session) and the two
// pages to dock.

import * as http from "node:http";
import * as vscode from "vscode";

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
    getTreeItem(item: vscode.TreeItem): vscode.TreeItem {
        return item;
    }

    getChildren(element?: vscode.TreeItem): vscode.TreeItem[] {
        if (element) {
            return [];
        }
        return [
            pageItem("control page", "mark4.openControl", "browser"),
            pageItem("plots page", "mark4.openPlots", "graph-line"),
        ];
    }
}
