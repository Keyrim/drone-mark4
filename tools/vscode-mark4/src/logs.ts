// The "Mark4 Logs" output channel: every Log envelope the gateway forwards,
// one line, through the method matching its level so VS Code owns the filter
// and the colors. The gateway's own lines arrive as frames from its node id
// like everyone else's, so nothing here knows about the hub.

import * as vscode from "vscode";

import { type NodeTable } from "./gen/gateway_pb";
import { type Envelope, LogLevel, NodeKind } from "./gen/mark4_pb";
import { formatLogLine } from "./logTree";
import { kindName } from "./model";

interface NodeNames {
    kind: string;
    modules: Map<number, string>;
}

export class LogChannel {
    private readonly channel = vscode.window.createOutputChannel("Mark4 Logs", { log: true });
    private readonly names = new Map<number, NodeNames>();

    dispose(): void {
        this.channel.dispose();
    }

    /** The table names the kinds and the modules the lines refer to. */
    setTable(table: NodeTable): void {
        this.names.clear();
        for (const node of table.nodes) {
            this.names.set(node.id, {
                kind: kindName(node.announce?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED),
                modules: new Map(node.logModules.map((module) => [module.id, module.name])),
            });
        }
    }

    /** One line of its own when the link came back: the gap is the news. */
    noteReconnect(): void {
        this.channel.info("reconnected to the gateway");
    }

    /** Writes the line of one Log envelope; every other body is ignored. */
    write(src: number, envelope: Envelope): void {
        if (envelope.body.case !== "log") {
            return;
        }
        const record = envelope.body.value;
        const known = this.names.get(src);
        const line = formatLogLine(
            known?.kind ?? "?",
            src,
            known?.modules.get(record.moduleId) ?? `#${record.moduleId}`,
            record.text,
        );
        switch (record.level) {
            case LogLevel.TRACE:
                this.channel.trace(line);
                return;
            case LogLevel.DEBUG:
                this.channel.debug(line);
                return;
            case LogLevel.WARN:
                this.channel.warn(line);
                return;
            case LogLevel.ERROR:
                this.channel.error(line);
                return;
            default:
                this.channel.info(line);
                return;
        }
    }
}
