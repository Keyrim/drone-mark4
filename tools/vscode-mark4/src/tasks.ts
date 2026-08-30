// Every action delegates to the repo scripts, so the extension, the command
// line and tasks.json share one build implementation. Tasks carry a mark4
// definition so their running executions can be found and terminated.

import { execFile } from "node:child_process";
import * as path from "node:path";
import * as vscode from "vscode";

import { simNodeId } from "./model";

export type Action = "build" | "run" | "godot";

/** The audit trail: every action and task event lands here, timestamped. */
export const log = vscode.window.createOutputChannel("Mark4", { log: true });

function folder(): vscode.WorkspaceFolder {
    const first = vscode.workspace.workspaceFolders?.[0];
    if (!first) {
        throw new Error("mark4: no workspace folder open");
    }
    return first;
}

function make(
    action: Action,
    app: string | undefined,
    execution: vscode.ShellExecution,
    matchers: string[],
    presentation: vscode.TaskPresentationOptions,
): vscode.Task {
    const label = app === undefined ? action : `${action} ${app}`;
    const task = new vscode.Task({ type: "mark4", action, app }, folder(), label, "mark4", execution, matchers);
    task.isBackground = action !== "build";
    task.presentationOptions = presentation;
    return task;
}

export function buildTask(app: string): vscode.Task {
    const script = path.join(folder().uri.fsPath, "scripts", "build_app.py");
    return make("build", app, new vscode.ShellExecution("python3", [script, app]), ["$gcc"], {
        reveal: vscode.TaskRevealKind.Always,
        panel: vscode.TaskPanelKind.Shared,
    });
}

export function runTask(app: string): vscode.Task {
    const script = path.join(folder().uri.fsPath, "scripts", "run_app.py");
    return make("run", app, new vscode.ShellExecution("python3", [script, app]), [], {
        reveal: vscode.TaskRevealKind.Always,
        panel: vscode.TaskPanelKind.Dedicated,
    });
}

/** The task name of one extension-started drone_sim instance. */
export function simTaskName(instance: number): string {
    return `drone_sim#${instance}`;
}

/**
 * One more drone_sim: its own emulated flash directory so the instances do
 * not share slots, and the node id the extension gave it so its line in the
 * nodes view knows which task to stop. Built through build_app.py like every
 * other run, then started directly because run_app.py passes no arguments.
 */
export function droneSimTask(instance: number): vscode.Task {
    const root = folder().uri.fsPath;
    const build = path.join(root, "scripts", "build_app.py");
    const binary = path.join(root, "software", "build", "desktop", "drone_sim", "drone_sim");
    const otaDirectory = path.join(root, "software", "build", "desktop", "drone_sim", `ota_flash_${instance}`);
    const command =
        `python3 '${build}' drone_sim && '${binary}'` +
        ` --node-id ${simNodeId(instance)} --ota-dir '${otaDirectory}'`;
    return make("run", simTaskName(instance), new vscode.ShellExecution(command), [], {
        reveal: vscode.TaskRevealKind.Always,
        panel: vscode.TaskPanelKind.Dedicated,
    });
}

/** The lowest instance number no running task holds. */
export function freeSimInstance(): number {
    let instance = 1;
    while (findExecution("run", simTaskName(instance)) !== undefined) {
        instance += 1;
    }
    return instance;
}

export function godotTask(): vscode.Task {
    const project = path.join(folder().uri.fsPath, "sim-godot");
    return make("godot", undefined, new vscode.ShellExecution("godot", ["--path", project]), [], {
        reveal: vscode.TaskRevealKind.Silent,
        panel: vscode.TaskPanelKind.Dedicated,
    });
}

export function findExecution(action: Action, app?: string): vscode.TaskExecution | undefined {
    // Match on source + name: the definition of a running execution does not
    // reliably round-trip through the task system, the label does.
    const label = app === undefined ? action : `${action} ${app}`;
    return vscode.tasks.taskExecutions.find(
        (execution) => execution.task.source === "mark4" && execution.task.name === label,
    );
}

/**
 * True when a godot sim process is alive, whoever started it. Task
 * executions do not survive a window reload but the task terminal and its
 * process do, so the process is the only truthful state.
 */
export function godotRunning(): Promise<boolean> {
    return new Promise((resolve) => {
        execFile("pgrep", ["-f", "godot --path"], (error) => resolve(error === null));
    });
}

/** Stops the godot sim, including one orphaned by a window reload. */
export function stopGodot(): void {
    log.info("stopGodot: terminating the task and pkilling the process");
    findExecution("godot")?.terminate();
    execFile("pkill", ["-f", "godot --path"], () => {});
}
