/**
 * Bench smoke of what the extension reads, without an editor: the same
 * GatewayClient the views use, against a running hub with a drone_sim.
 * Prints the node lines of the nodes view, the first log lines as the
 * output channel formats them, then moves sim/link to DEBUG, shows the
 * DEBUG lines that follow and queries the table back.
 *
 *   pnpm smoke               # against ws://127.0.0.1:47810
 *   HUB_URL=ws://host:port pnpm smoke
 *
 * Exits non-zero on the first failed expectation.
 */

import { type NodeTable } from "../src/gen/gateway_pb";
import { LogLevel, NodeKind } from "../src/gen/mark4_pb";
import { GatewayClient, GATEWAY_URL } from "../src/gateway";
import { formatLogLine, levelName } from "../src/logTree";
import { hexNodeId, kindName, nodeRows } from "../src/model";

const startedAt = Date.now();
const log = (text: string): void => console.log(`[${((Date.now() - startedAt) / 1000).toFixed(2)} s] ${text}`);
const fail = (text: string): never => {
    console.error(`FAIL: ${text}`);
    process.exit(1);
};

let table: NodeTable | undefined;
let wireHash = 0;
const kinds = new Map<number, string>();
const modules = new Map<number, Map<number, string>>();
let printed = 0;
let debugLines = 0;
let watchDebug = 0;

const client = new GatewayClient({
    onNodes: (received) => {
        table = received;
        kinds.clear();
        modules.clear();
        for (const node of received.nodes) {
            kinds.set(node.id, kindName(node.announce?.kind ?? NodeKind.NODE_KIND_UNSPECIFIED));
            modules.set(node.id, new Map(node.logModules.map((module) => [module.id, module.name])));
        }
    },
    onStatus: (status) => {
        wireHash = status.wireHash;
    },
    onEnvelope: (src, envelope) => {
        if (envelope.body.case !== "log") {
            return;
        }
        const record = envelope.body.value;
        const line = formatLogLine(
            kinds.get(src) ?? "?",
            src,
            modules.get(src)?.get(record.moduleId) ?? `#${record.moduleId}`,
            record.text,
        );
        if (printed < 10 && watchDebug === 0) {
            printed += 1;
            log(`${levelName(record.level).padEnd(5)} ${line}`);
        }
        if (watchDebug === src && record.level === LogLevel.DEBUG) {
            debugLines += 1;
            if (debugLines <= 5) {
                log(`DEBUG ${line}`);
            }
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

log(`log lines (kind | node | module | text), ${sim.logModules.length} modules known for ${hexNodeId(sim.id)}:`);
// A healthy bench is quiet: whatever comes in the window is what there is.
await sleep(5000);
log(`${printed} line(s) in 5 s`);

const link = sim.logModules.find((module) => module.name === "sim/link") ?? fail("no sim/link module on the sim");
watchDebug = sim.id;
client.setLogLevel(sim.id, link.id, LogLevel.DEBUG);
log(`sent sim/link -> DEBUG to ${hexNodeId(sim.id)}, listening 3 s`);
await sleep(3000);
if (debugLines === 0) {
    fail("no DEBUG line after the set (is the plant driving the sim?)");
}
log(`${debugLines} DEBUG lines`);

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
