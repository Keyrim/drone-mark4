/**
 * The editor's theme, relayed into a page it embeds.
 *
 * The editor injects its theme as --vscode-* custom properties on the <html>
 * element of the webview document, and names the theme as a class on its
 * <body> (vscode-dark, vscode-light, vscode-high-contrast,
 * vscode-high-contrast-light). A page the hub serves runs in an iframe of
 * another origin, which inherits neither, so the webview host posts both as
 * one message and this module writes them where the page and its components
 * read them.
 *
 * Pure DOM, no socket: a page opened in a browser tab never gets the message
 * and keeps the defaults its own stylesheet carries.
 */

/** The theme kinds the editor names, as they arrive and as they are worn. */
const KINDS = ["vscode-dark", "vscode-light", "vscode-high-contrast", "vscode-high-contrast-light"];

/** What the webview host posts to the iframe on every theme change. */
export interface ThemeMessage {
    readonly type: "mark4-theme";
    /** One of KINDS; anything else is ignored. */
    readonly kind: string;
    /** Every --vscode-* property name mapped to its value. */
    readonly vars: Record<string, string>;
}

/** True when a posted message is a theme and nothing else. */
export function isThemeMessage(data: unknown): data is ThemeMessage {
    if (typeof data !== "object" || data === null) {
        return false;
    }
    const message = data as { type?: unknown; kind?: unknown; vars?: unknown };
    return (
        message.type === "mark4-theme" &&
        typeof message.kind === "string" &&
        typeof message.vars === "object" &&
        message.vars !== null
    );
}

/** Wears one theme: its variables on the root element, its kind on the body. */
export function applyTheme(message: ThemeMessage): void {
    const style = document.documentElement.style;
    for (const [name, value] of Object.entries(message.vars)) {
        // The theme's own properties only: what a page defines on :root is
        // its default, and nothing else is the host's to write.
        if (name.startsWith("--vscode-") && typeof value === "string") {
            style.setProperty(name, value);
        }
    }
    if (KINDS.includes(message.kind)) {
        document.body.classList.remove(...KINDS);
        document.body.classList.add(message.kind);
    }
}

/** Takes the host's theme messages from now on. One call per page. */
export function installTheme(): void {
    window.addEventListener("message", (event: MessageEvent) => {
        if (isThemeMessage(event.data)) {
            applyTheme(event.data);
        }
    });
}
