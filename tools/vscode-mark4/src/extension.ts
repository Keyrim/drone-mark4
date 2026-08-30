// Thin bench tooling: the apps tree shells out to the repo build scripts,
// the nodes and log levels views read the gateway's websocket, the pages
// open as webviews around an iframe on the hub URL.

import * as vscode from "vscode";

import { AppItem, AppsProvider } from "./appsTree";
import { BenchProvider, HUB_URL, pingHub, waitForHub } from "./bench";
import { GatewayClient } from "./gateway";
import { NodeKind } from "./gen/mark4_pb";
import { LevelTreeItem, LogLevelsProvider } from "./logLevelsTree";
import { LogChannel } from "./logs";
import { LEVEL_NAMES } from "./logTree";
import { simInstance } from "./model";
import { NodeItem, NodesProvider } from "./nodesTree";
import {
    buildTask,
    droneSimTask,
    findExecution,
    freeSimInstance,
    godotRunning,
    godotTask,
    log,
    runTask,
    simTaskName,
    stopGodot,
} from "./tasks";
import { openPage } from "./webviews";

async function startHub(): Promise<void> {
    if ((await pingHub()) || findExecution("run", "hub")) {
        void vscode.window.showInformationMessage(`mark4: a hub is already up or starting on ${HUB_URL}`);
        return;
    }
    await vscode.tasks.executeTask(runTask("hub"));
}

async function startGodot(): Promise<void> {
    if (!(await godotRunning())) {
        await vscode.tasks.executeTask(godotTask());
    }
}

/** Stops the local process behind a node, when the extension owns one. */
function stopNode(item: NodeItem): void {
    log.info(`stopNode: ${item.row.kindName} ${item.row.hex}`);
    if (item.row.kind === NodeKind.PLANT) {
        stopGodot();
        return;
    }
    if (item.row.kind === NodeKind.GATEWAY) {
        findExecution("run", "hub")?.terminate();
        return;
    }
    const instance = simInstance(item.row.id);
    if (instance !== undefined) {
        findExecution("run", simTaskName(instance))?.terminate();
    }
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
    const nodes = new NodesProvider();
    const levels = new LogLevelsProvider();
    const logs = new LogChannel();
    const gateway = new GatewayClient({
        onNodes: (table) => {
            nodes.setTable(table);
            levels.setTable(table);
            logs.setTable(table);
        },
        onStatus: (status) => nodes.setStatus(status),
        onEnvelope: (src, envelope) => logs.write(src, envelope),
        onState: (open, reconnected) => {
            log.info(`gateway link ${open ? "open" : "closed"}`);
            nodes.setOnline(open);
            if (!open) {
                levels.setTable(undefined);
            } else if (reconnected) {
                logs.noteReconnect();
            }
        },
    });

    context.subscriptions.push(
        new vscode.Disposable(() => gateway.dispose()),
        logs,
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
        vscode.window.registerTreeDataProvider("mark4.bench", new BenchProvider()),
        vscode.window.registerTreeDataProvider("mark4.nodes", nodes),
        vscode.window.registerTreeDataProvider("mark4.logLevels", levels),
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
        vscode.commands.registerCommand("mark4.startGodot", startGodot),
        vscode.commands.registerCommand("mark4.stopGodot", stopGodot),
        vscode.commands.registerCommand("mark4.addDroneSim", () => {
            const instance = freeSimInstance();
            log.info(`addDroneSim: instance ${instance}`);
            return vscode.tasks.executeTask(droneSimTask(instance));
        }),
        vscode.commands.registerCommand("mark4.stopNode", stopNode),
        vscode.commands.registerCommand("mark4.toggleLogGrouping", () => {
            log.info(`log levels grouped ${levels.toggleMode()}`);
        }),
        vscode.commands.registerCommand("mark4.setLogLevel", async (item: LevelTreeItem) => {
            const picked = await vscode.window.showQuickPick([...LEVEL_NAMES], {
                placeHolder: `level of ${item.item.label} (${item.item.targets.length} module(s))`,
            });
            if (picked === undefined) {
                return;
            }
            const level = LEVEL_NAMES.indexOf(picked as (typeof LEVEL_NAMES)[number]);
            log.info(`setLogLevel: ${item.item.key} -> ${picked}`);
            for (const target of item.item.targets) {
                gateway.setLogLevel(target.nodeId, target.moduleId, level);
            }
            for (const nodeId of new Set(item.item.targets.map((target) => target.nodeId))) {
                gateway.queryLogModules(nodeId);
            }
        }),
        vscode.commands.registerCommand("mark4.openControl", () => openPage("control", vscode.ViewColumn.Active)),
        vscode.commands.registerCommand("mark4.openPlots", () => openPage("plots", vscode.ViewColumn.Active)),
        vscode.commands.registerCommand("mark4.benchSession", benchSession),
    );
}
