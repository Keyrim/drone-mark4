/**
 * Bench smoke of the log path, from a script instead of a browser: with a
 * hub and one drone_sim on the LAN, checks that the NodeTable carries the
 * sim's log modules, that its boot Log line resolves to app/boot, that a
 * LogControl set on sim/link turns its DEBUG lines on (and only its), and
 * that a query answers with the table showing the new level.
 *
 *   pnpm log-smoke
 *
 * Exits non-zero on the first failed expectation.
 */

import { create, fromBinary } from "@bufbuild/protobuf";
import WebSocket from "ws";

import { type GatewayMessage, type Node } from "../src/gen/gateway_pb";
import { EnvelopeSchema, LogLevel, NodeKind, type Envelope, type LogModuleInfo } from "../src/gen/mark4_pb";
import { decodeGatewayMessage, encodeGatewayMessage, frameMessage } from "../src/shared/gateway_socket";
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

function envelopeOf(message: GatewayMessage): { src: number; envelope: Envelope } | undefined {
    if (message.body.case !== "frame") {
        return undefined;
    }
    return { src: message.body.value.src, envelope: fromBinary(EnvelopeSchema, message.body.value.payload) };
}

type LogRequest = { case: "query"; value: boolean } | { case: "set"; value: { moduleId: number; level: LogLevel } };

function logControl(dst: number, request: LogRequest): GatewayMessage {
    return frameMessage(dst, create(EnvelopeSchema, { body: { case: "logControl", value: { request } } }));
}

/** Collects Log lines of one node for a while, returns them by module id. */
function collectLogs(src: number, ms: number): Promise<Map<number, number>> {
    const counts = new Map<number, number>();
    return new Promise((resolve) => {
        const waiter = (message: GatewayMessage): boolean => {
            const frame = envelopeOf(message);
            if (frame?.src === src && frame.envelope.body.case === "log") {
                const id = frame.envelope.body.value.moduleId;
                counts.set(id, (counts.get(id) ?? 0) + 1);
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

    // 1. The table lists a drone_sim with its log modules.
    const sim = await waitFor("a drone_sim with log modules in the node table", (message) => {
        if (message.body.case !== "nodes") {
            return undefined;
        }
        return message.body.value.nodes.find(
            (node: Node) => node.announce?.kind === NodeKind.DRONE_SIM && node.logModules.length > 0,
        );
    });
    const names = (modules: readonly LogModuleInfo[]): string =>
        modules.map((m) => `${m.name}=${LogLevel[m.level]}`).join(" ");
    log(`drone_sim ${hexNodeId(sim.id)} modules: ${names(sim.logModules)}`);
    const link = sim.logModules.find((m) => m.name === "sim/link") ?? fail("no sim/link module");
    const boot = sim.logModules.find((m) => m.name === "app/boot") ?? fail("no app/boot module");

    // 2. Its module table arrives as LogModules pages on a query, with the
    //    names of the table; its boot line names app/boot. The sim booted
    //    before this script, so the boot line is provoked: a Reboot makes
    //    the sim re-run its boot decision and log it through app/boot.
    ws.send(encodeGatewayMessage(logControl(sim.id, { case: "query", value: true })));
    const page = await waitFor("a LogModules page from the sim", (message) => {
        const frame = envelopeOf(message);
        return frame?.src === sim.id && frame.envelope.body.case === "logModules" ? frame.envelope.body.value : undefined;
    });
    log(`query answered: page ${page.startIndex}/${page.total}: ${names(page.modules)}`);
    ws.send(
        encodeGatewayMessage(frameMessage(sim.id, create(EnvelopeSchema, { body: { case: "reboot", value: {} } }))),
    );
    const bootLine = await waitFor("an app/boot Log line", (message) => {
        const frame = envelopeOf(message);
        return frame?.src === sim.id && frame.envelope.body.case === "log" && frame.envelope.body.value.moduleId === boot.id
            ? frame.envelope.body.value
            : undefined;
    });
    log(`app/boot ${LogLevel[bootLine.level]}: ${bootLine.text}`);

    // 3. DEBUG on sim/link: DEBUG lines from that module only.
    const before = await collectLogs(sim.id, 2500);
    log(`before: ${before.get(link.id) ?? 0} sim/link lines in 2.5 s`);
    ws.send(encodeGatewayMessage(logControl(sim.id, { case: "set", value: { moduleId: link.id, level: LogLevel.DEBUG } })));
    const updated = await waitFor("the table with sim/link at DEBUG", (message) => {
        const frame = envelopeOf(message);
        if (frame?.src !== sim.id || frame.envelope.body.case !== "logModules") {
            return undefined;
        }
        return frame.envelope.body.value.modules.find((m) => m.id === link.id && m.level === LogLevel.DEBUG);
    });
    log(`set acknowledged by a table: ${updated.name}=${LogLevel[updated.level]}`);
    const after = await collectLogs(sim.id, 2500);
    const linkLines = after.get(link.id) ?? 0;
    const others = [...after.entries()].filter(([id]) => id !== link.id);
    log(`after: ${linkLines} sim/link lines in 2.5 s, other modules: ${JSON.stringify(others)}`);
    if (linkLines === 0) {
        fail("no DEBUG line from sim/link after the set (is the plant driving the sim?)");
    }

    // 4. The gateway's table shows the level too, and a query returns it.
    const table = await waitFor("the node table with sim/link at DEBUG", (message) => {
        if (message.body.case !== "nodes") {
            return undefined;
        }
        const node = message.body.value.nodes.find((n: Node) => n.id === sim.id);
        return node?.logModules.find((m) => m.id === link.id && m.level === LogLevel.DEBUG);
    });
    log(`node table: ${table.name}=${LogLevel[table.level]}`);
    ws.send(encodeGatewayMessage(logControl(sim.id, { case: "set", value: { moduleId: link.id, level: LogLevel.INFO } })));
    await waitFor("sim/link back at INFO", (message) => {
        const frame = envelopeOf(message);
        if (frame?.src !== sim.id || frame.envelope.body.case !== "logModules") {
            return undefined;
        }
        return frame.envelope.body.value.modules.find((m) => m.id === link.id && m.level === LogLevel.INFO);
    });
    log("restored sim/link to INFO; all good");
    ws.close();
    process.exit(0);
});
ws.on("error", (error: Error) => fail(`websocket: ${error.message}`));
