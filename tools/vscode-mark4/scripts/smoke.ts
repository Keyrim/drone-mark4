/**
 * Bench smoke of what the extension reads, without an editor: the same
 * GatewayClient the views use, against a running hub with a drone_sim.
 * Prints the node lines of the nodes view, then the same stored log lines
 * twice, once with no node table (names unresolved) and once with it (the
 * retroactive resolution the channel does on every redraw), then moves
 * sim/link to DEBUG on the node and in the view and shows what follows.
 *
 *   pnpm smoke               # against ws://127.0.0.1:47810
 *   HUB_URL=ws://host:port pnpm smoke
 *
 * Exits non-zero on the first failed expectation.
 */

import { type NodeTable } from "../src/gen/gateway_pb";
import { LogLevel, NodeKind } from "../src/gen/mark4_pb";
import { GatewayClient, GATEWAY_URL } from "../src/gateway";
import { LogStore } from "../src/logStore";
import { levelName } from "../src/logTree";
import { LogFilter, type NameTable, type NodeNames, renderLogs } from "../src/logView";
import { hexNodeId, kindName, nodeRows } from "../src/model";

const startedAt = Date.now();
const log = (text: string): void => console.log(`[${((Date.now() - startedAt) / 1000).toFixed(2)} s] ${text}`);
const fail = (text: string): never => {
    console.error(`FAIL: ${text}`);
    process.exit(1);
};

let table: NodeTable | undefined;
let wireHash = 0;
let names: NameTable = new Map();
const store = new LogStore();
const filter = new LogFilter();
let debugLines = 0;
let watchDebug = 0;

/** The first lines of a projection, as the output channel would print them. */
const showLines = (text: string, count = 6): void => {
    const lines = text === "" ? [] : text.split("\n");
    for (const line of lines.slice(0, count)) {
        log(`  ${line}`);
    }
    log(`  (${lines.length} line(s) shown of ${store.size} stored)`);
};

const client = new GatewayClient({
    onNodes: (received) => {
        table = received;
        names = new Map<number, NodeNames>(
            received.nodes.map((node) => [
                node.id,
                {
                    kind: kindName(node.announce?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED),
                    modules: new Map(node.logModules.map((module) => [module.id, module.name])),
                },
            ]),
        );
    },
    onStatus: (status) => {
        wireHash = status.wireHash;
    },
    onEnvelope: (src, envelope) => {
        if (envelope.body.case !== "log") {
            return;
        }
        const record = envelope.body.value;
        // Nothing is formatted at ingest: the store keeps the raw record and
        // the extension's own clock, the names are resolved at render time.
        store.push({
            receivedAt: new Date(),
            nodeId: src,
            moduleId: record.moduleId,
            level: record.level,
            text: record.text,
        });
        if (watchDebug === src && record.level === LogLevel.DEBUG) {
            debugLines += 1;
        }
    },
    onState: (open, reconnected) => log(`link ${open ? "open" : "closed"}${reconnected ? " (reconnected)" : ""}`),
}, process.env["HUB_URL"] ?? GATEWAY_URL);

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor<T>(what: string, pick: () => T | undefined, timeoutMs = 20000): Promise<T> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        const value = pick();
        if (value !== undefined) {
            return value;
        }
        if (Date.now() > deadline) {
            return fail(`timeout waiting for ${what}`);
        }
        await sleep(200);
    }
}

const sim = await waitFor("a drone_sim with its log modules in the node table", () =>
    table?.nodes.find((node) => node.announce?.kind === NodeKind.DRONE_SIM && node.logModules.length > 0),
);
log(`nodes view (gateway wire ${wireHash.toString(16).padStart(8, "0")}):`);
for (const row of nodeRows(table?.nodes ?? [], wireHash)) {
    log(`  ${row.live ? "*" : "."} ${row.name} [${row.kindName} ${row.hex}]${row.mismatch ? " WIRE MISMATCH" : ""}`);
}

const link = sim.logModules.find((module) => module.name === "sim/link") ?? fail("no sim/link module on the sim");
watchDebug = sim.id;
// One gesture, two effects: the node is moved and so is what the view shows
// (a level raised on the node alone would print nothing: the display filter
// starts at INFO).
client.setLogLevel(sim.id, link.id, LogLevel.DEBUG);
filter.setLevel([{ nodeId: sim.id, moduleId: link.id }], LogLevel.DEBUG);
log(`sent sim/link -> DEBUG to ${hexNodeId(sim.id)}, storing 5 s of lines`);
await sleep(5000);
if (debugLines === 0) {
    fail("no DEBUG line after the set (is the plant driving the sim?)");
}
// The store holds raw records: the same ones read differently once the node
// table is known, which is what a late Announce does to the lines before it.
log("with no node table, the kind and the module are unresolved:");
showLines(renderLogs(store.records(), new Map(), filter));
log("with the table, every stored line is named again:");
showLines(renderLogs(store.records(), names, filter));

client.queryLogModules(sim.id);
const refreshed = await waitFor("the table with sim/link at DEBUG", () =>
    table?.nodes
        .find((node) => node.id === sim.id)
        ?.logModules.find((module) => module.id === link.id && module.level === LogLevel.DEBUG),
);
log(`query answered: ${refreshed.name} = ${levelName(refreshed.level)}`);

client.setLogLevel(sim.id, link.id, LogLevel.INFO);
client.queryLogModules(sim.id);
await waitFor("sim/link back at INFO", () =>
    table?.nodes
        .find((node) => node.id === sim.id)
        ?.logModules.find((module) => module.id === link.id && module.level === LogLevel.INFO),
);
log("restored sim/link to INFO; all good");
client.dispose();
process.exit(0);
