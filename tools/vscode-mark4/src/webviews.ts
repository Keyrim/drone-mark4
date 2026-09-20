// Each page is a WebviewPanel around one full-frame iframe on the hub. The
// iframe is load-bearing: the pages reach the hub with ws://location.host,
// which must stay the hub origin, not the webview one. The URL is used as
// is, never through asExternalUri: the client reaches 127.0.0.1 directly
// (WSL2), and a VS Code port forward would hold 47810 after the hub dies
// and block its restart (this repo already opts out of auto forwarding).

import * as vscode from "vscode";
import { HUB_URL } from "./bench";

const PAGES = {
    control: { title: "mark4 control", path: "" },
    plots: { title: "mark4 plots", path: "plots.html" },
    transport: { title: "mark4 transport", path: "transport.html" },
} as const;

const panels = new Map<string, vscode.WebviewPanel>();

export function openPage(page: keyof typeof PAGES, column: vscode.ViewColumn): void {
    const existing = panels.get(page);
    if (existing) {
        existing.reveal(column);
        return;
    }
    const panel = vscode.window.createWebviewPanel(`mark4.${page}`, PAGES[page].title, column, {
        retainContextWhenHidden: true,
        enableScripts: true,
    });
    panel.webview.html = `<!DOCTYPE html>
<html>
<head>
    <style>
        html, body { margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; }
        iframe { width: 100%; height: 100%; border: 0; }
    </style>
</head>
<body>
<iframe src="${HUB_URL}/${PAGES[page].path}"></iframe>
<script>
    // The page forwards shortcut keydowns it cannot deliver itself (key
    // events never leave an iframe); re-dispatching them here puts them
    // where the editor keybinding service listens. keyCode is defined by
    // hand: the constructor ignores it and the editor still reads it.
    window.addEventListener("message", (event) => {
        const data = event.data;
        if (!data || data.type !== "mark4-shortcut") {
            return;
        }
        const synthetic = new KeyboardEvent("keydown", {
            key: data.key,
            code: data.code,
            ctrlKey: data.ctrlKey,
            shiftKey: data.shiftKey,
            altKey: data.altKey,
            metaKey: data.metaKey,
            repeat: data.repeat,
            bubbles: true,
        });
        Object.defineProperty(synthetic, "keyCode", { get: () => data.keyCode });
        document.dispatchEvent(synthetic);
    });

    // The editor gives this document its theme as --vscode-* properties on
    // <html> and one vscode-* class on <body>; an iframe on another origin
    // is given neither, so the host reads both and posts them to the page,
    // which applies them itself. Sent when the page loads and again on every
    // theme change (the editor rewrites the properties and the class).
    const iframe = document.querySelector("iframe");
    const hubOrigin = new URL(iframe.src).origin;
    const THEME_KINDS = ["vscode-high-contrast-light", "vscode-high-contrast", "vscode-light", "vscode-dark"];
    const postTheme = () => {
        const style = document.documentElement.style;
        const vars = {};
        for (let index = 0; index < style.length; index++) {
            const name = style.item(index);
            if (name.startsWith("--vscode-")) {
                vars[name] = style.getPropertyValue(name);
            }
        }
        const kind = THEME_KINDS.find((candidate) => document.body.classList.contains(candidate)) || "vscode-dark";
        iframe.contentWindow?.postMessage({ type: "mark4-theme", kind: kind, vars: vars }, hubOrigin);
    };
    iframe.addEventListener("load", postTheme);
    const themeObserver = new MutationObserver(postTheme);
    themeObserver.observe(document.documentElement, { attributes: true, attributeFilter: ["style"] });
    themeObserver.observe(document.body, { attributes: true, attributeFilter: ["class"] });
</script>
</body>
</html>`;
    panel.onDidDispose(() => panels.delete(page));
    panels.set(page, panel);
}
