/**
 * Bench smoke of the log path, from a script instead of a browser: with a
 * hub and one drone_sim on the LAN, checks that the gateway publishes the
 * sim's module table and its lines (the ring on connect, then one message
 * per line), that its boot line resolves to app/boot, and that a
 * LogCommand.set_level on sim/link turns its DEBUG lines on (and only
 * its) and comes back in the published module table.
 *
 *   pnpm log-smoke
 *
 * Exits non-zero on the first failed expectation.
 */

import { create } from "@bufbuild/protobuf";
import WebSocket from "ws";

import { GatewayMessageSchema, type GatewayMessage, type Node } from "../src/gen/gateway_pb";
import { LogLevel, NodeKind, type LogModuleInfo } from "../src/gen/mark4_pb";
import { decodeGatewayMessage, encodeGatewayMessage } from "../src/shared/gateway_socket";
import { hexNodeId } from "../src/shared/nodes";

const URL = process.env["HUB_URL"] ?? "ws://127.0.0.1:47810";
const startedAt = Date.now();
const log = (text: string): void => console.log(`[${((Date.now() - startedAt) / 1000).toFixed(2)} s] ${text}`);
const fail = (text: string): never => {
    console.error(`FAIL: ${text}`);
    process.exit(1);
};

const ws = new WebSocket(URL);
ws.binaryType = "arraybuffer";
const waiters: ((message: GatewayMessage) => boolean)[] = [];
ws.on("message", (data: ArrayBuffer) => {
    const message = decodeGatewayMessage(new Uint8Array(data));
    if (message === null) {
        return fail("undecodable websocket message");
    }
    for (const waiter of [...waiters]) {
        if (waiter(message)) {
            waiters.splice(waiters.indexOf(waiter), 1);
        }
    }
});

function waitFor<T>(what: string, pick: (message: GatewayMessage) => T | undefined, timeoutMs = 15000): Promise<T> {
    return new Promise((resolve) => {
        const timer = setTimeout(() => fail(`timeout waiting for ${what}`), timeoutMs);
        waiters.push((message) => {
            const value = pick(message);
            if (value === undefined) {
                return false;
            }
            clearTimeout(timer);
            resolve(value);
            return true;
        });
    });
}

/** The module table of one node, when this message is one. */
function modulesOf(message: GatewayMessage, node: number): readonly LogModuleInfo[] | undefined {
    return message.body.case === "nodeLogModules" && message.body.value.node === node
        ? message.body.value.modules
        : undefined;
}

/** The lines of one node, when this message carries some. */
function linesOf(message: GatewayMessage, node: number) {
    return message.body.case === "nodeLogLines" && message.body.value.node === node
        ? message.body.value.lines
        : undefined;
}

/** The module table pulled again. */
function logRefresh(node: number): GatewayMessage {
    return create(GatewayMessageSchema, {
        body: { case: "logCommand", value: { node, action: { case: "refresh", value: true } } },
    });
}

/** One module moved to one level. */
function logSetLevel(node: number, moduleId: number, level: LogLevel): GatewayMessage {
    return create(GatewayMessageSchema, {
        body: {
            case: "logCommand",
            value: { node, action: { case: "setLevel", value: { moduleId, level } } },
        },
    });
}

/** One node command: a reboot. */
function reboot(node: number): GatewayMessage {
    return create(GatewayMessageSchema, {
        body: { case: "nodeCommand", value: { node, action: { case: "reboot", value: true } } },
    });
}

/** Collects the lines of one node for a while, returns them by module id. */
function collectLogs(src: number, ms: number): Promise<Map<number, number>> {
    const counts = new Map<number, number>();
    return new Promise((resolve) => {
        const waiter = (message: GatewayMessage): boolean => {
            for (const line of linesOf(message, src) ?? []) {
                counts.set(line.moduleId, (counts.get(line.moduleId) ?? 0) + 1);
            }
            return false;
        };
        waiters.push(waiter);
        setTimeout(() => {
            waiters.splice(waiters.indexOf(waiter), 1);
            resolve(counts);
        }, ms);
    });
}

ws.on("open", async () => {
    log(`connected to ${URL}`);

    // 1. The node table lists a drone_sim, and the gateway publishes its
    //    module table as a message of its own.
    const sim = await waitFor("a drone_sim in the node table", (message) => {
        if (message.body.case !== "nodes") {
            return undefined;
        }
        return message.body.value.nodes.find((node: Node) => node.announce?.kind === NodeKind.DRONE_SIM);
    });
    const names = (modules: readonly LogModuleInfo[]): string =>
        modules.map((m) => `${m.name}=${LogLevel[m.level]}`).join(" ");
    const published = await waitFor(
        `the log modules of ${hexNodeId(sim.id)}`,
        (message) => {
            const modules = modulesOf(message, sim.id);
            return modules !== undefined && modules.length > 0 ? modules : undefined;
        },
    );
    log(`drone_sim ${hexNodeId(sim.id)} modules: ${names(published)}`);
    const link = published.find((m) => m.name === "sim/link") ?? fail("no sim/link module");
    const boot = published.find((m) => m.name === "app/boot") ?? fail("no app/boot module");

    // 2. The table comes again on a refresh, and the sim's boot line names
    //    app/boot. The sim booted before this script, so the boot line is
    //    provoked: a reboot makes it re-run its boot decision and log it.
    ws.send(encodeGatewayMessage(logRefresh(sim.id)));
    const again = await waitFor("the module table published again", (message) => {
        const modules = modulesOf(message, sim.id);
        return modules !== undefined && modules.length > 0 ? modules : undefined;
    });
    log(`refresh answered: ${again.length} modules: ${names(again)}`);
    ws.send(encodeGatewayMessage(reboot(sim.id)));
    const bootLine = await waitFor("an app/boot log line", (message) =>
        (linesOf(message, sim.id) ?? []).find((line) => line.moduleId === boot.id),
    );
    log(`app/boot ${LogLevel[bootLine.level]}: ${bootLine.text}`);

    // 3. DEBUG on sim/link: DEBUG lines from that module only.
    const before = await collectLogs(sim.id, 2500);
    log(`before: ${before.get(link.id) ?? 0} sim/link lines in 2.5 s`);
    ws.send(encodeGatewayMessage(logSetLevel(sim.id, link.id, LogLevel.DEBUG)));
    const updated = await waitFor("sim/link published at DEBUG", (message) =>
        (modulesOf(message, sim.id) ?? []).find((m) => m.id === link.id && m.level === LogLevel.DEBUG),
    );
    log(`set answered by the module: ${updated.name}=${LogLevel[updated.level]}`);
    const after = await collectLogs(sim.id, 2500);
    const linkLines = after.get(link.id) ?? 0;
    const others = [...after.entries()].filter(([id]) => id !== link.id);
    log(`after: ${linkLines} sim/link lines in 2.5 s, other modules: ${JSON.stringify(others)}`);
    if (linkLines === 0) {
        fail("no DEBUG line from sim/link after the set (is the plant driving the sim?)");
    }

    // 4. A refresh shows the level in the whole table too.
    ws.send(encodeGatewayMessage(logRefresh(sim.id)));
    const table = await waitFor("the module table with sim/link at DEBUG", (message) =>
        (modulesOf(message, sim.id) ?? []).find((m) => m.id === link.id && m.level === LogLevel.DEBUG),
    );
    log(`module table: ${table.name}=${LogLevel[table.level]}`);
    ws.send(encodeGatewayMessage(logSetLevel(sim.id, link.id, LogLevel.INFO)));
    await waitFor("sim/link back at INFO", (message) =>
        (modulesOf(message, sim.id) ?? []).find((m) => m.id === link.id && m.level === LogLevel.INFO),
    );
    log("restored sim/link to INFO; all good");
    ws.close();
    process.exit(0);
});
ws.on("error", (error: Error) => fail(`websocket: ${error.message}`));
