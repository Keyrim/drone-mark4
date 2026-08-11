// Thin bench tooling: the apps tree shells out to the repo build scripts,
// the bench view starts and stops the hub and the Godot sim as tasks, the
// pages open as webviews around an iframe on the hub URL. The extension
// knows the hub URL and nothing else: no wire structs, no JSON.

import * as vscode from "vscode";
import { AppItem, AppsProvider } from "./appsTree";
import { BenchProvider, HUB_URL, pingHub, waitForHub } from "./bench";
import { buildTask, findExecution, godotRunning, godotTask, log, runTask, stopGodot } from "./tasks";
import { openPage } from "./webviews";

async function startHub(): Promise<void> {
    if ((await pingHub()) || findExecution("run", "hub")) {
        void vscode.window.showInformationMessage(`mark4: a hub is already up or starting on ${HUB_URL}`);
        return;
    }
    await vscode.tasks.executeTask(runTask("hub"));
}

let sessionActive = false;

async function benchSession(): Promise<void> {
    if (sessionActive) {
        return;
    }
    sessionActive = true;
    try {
        await benchSessionInner();
    } finally {
        sessionActive = false;
    }
}

async function benchSessionInner(): Promise<void> {
    const hubUp = await pingHub();
    const hubExecution = findExecution("run", "hub") !== undefined;
    log.info(`benchSession: hub ping ${hubUp}, hub task ${hubExecution}`);
    if (!hubUp && !hubExecution) {
        log.info("benchSession: starting the hub task");
        await vscode.tasks.executeTask(runTask("hub"));
    }
    if (!(await godotRunning())) {
        log.info("benchSession: starting the godot task");
        await vscode.tasks.executeTask(godotTask());
    }
    const up = await vscode.window.withProgress(
        {
            location: vscode.ProgressLocation.Notification,
            title: `mark4: waiting for the hub on ${HUB_URL}`,
            cancellable: true,
        },
        (_progress, token) => waitForHub(300_000, token),
    );
    if (!up) {
        void vscode.window.showErrorMessage(`mark4: the hub never answered on ${HUB_URL}`);
        return;
    }
    openPage("control", vscode.ViewColumn.Active);
    openPage("plots", vscode.ViewColumn.Active);
}

export function activate(context: vscode.ExtensionContext): void {
    log.info("extension activated");
    context.subscriptions.push(
        vscode.tasks.onDidStartTask((event) =>
            log.info(`task started: [${event.execution.task.source}] ${event.execution.task.name}`),
        ),
        vscode.tasks.onDidEndTaskProcess((event) =>
            log.info(
                `task process ended: [${event.execution.task.source}] ${event.execution.task.name}` +
                    ` (exit ${event.exitCode ?? "unknown"})`,
            ),
        ),
        vscode.window.registerTreeDataProvider("mark4.apps", new AppsProvider(context)),
        vscode.window.registerTreeDataProvider("mark4.bench", new BenchProvider(context)),
        vscode.commands.registerCommand("mark4.buildApp", (item: AppItem) => {
            log.info(`buildApp: ${item.app.name}`);
            return vscode.tasks.executeTask(buildTask(item.app.name));
        }),
        vscode.commands.registerCommand("mark4.runApp", (item: AppItem) => {
            log.info(`runApp: ${item.app.name}`);
            return vscode.tasks.executeTask(runTask(item.app.name));
        }),
        vscode.commands.registerCommand("mark4.stopApp", (item: AppItem) => {
            log.info(`stopApp: ${item.app.name}`);
            findExecution("run", item.app.name)?.terminate();
        }),
        vscode.commands.registerCommand("mark4.debugApp", (item: AppItem) => {
            if (item.debugName) {
                void vscode.debug.startDebugging(vscode.workspace.workspaceFolders?.[0], item.debugName);
            }
        }),
        vscode.commands.registerCommand("mark4.startHub", startHub),
        vscode.commands.registerCommand("mark4.stopHub", () => {
            log.info("stopHub");
            findExecution("run", "hub")?.terminate();
        }),
        vscode.commands.registerCommand("mark4.startGodot", async () => {
            if (!(await godotRunning())) {
                await vscode.tasks.executeTask(godotTask());
            }
        }),
        vscode.commands.registerCommand("mark4.stopGodot", stopGodot),
        vscode.commands.registerCommand("mark4.openControl", () => openPage("control", vscode.ViewColumn.Active)),
        vscode.commands.registerCommand("mark4.openPlots", () => openPage("plots", vscode.ViewColumn.Active)),
        vscode.commands.registerCommand("mark4.benchSession", benchSession),
    );
}
