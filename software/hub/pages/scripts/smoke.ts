/**
 * Bench smoke of the gateway contract, from a script instead of a browser:
 * connects to a running hub over `ws`, and with a plant and flight processes
 * on the LAN checks that the node table lists them by id, that a NodeStatus
 * arrives from every drone (truth included), that a PilotInput for one drone
 * shows in its status, that the whole telemetry chain works (the table
 * published, a configuration answered by the configuration as applied, a
 * subscribe followed by samples and an unsubscribe by silence), that the
 * profile service answers, and optionally that an OTA start against a
 * drone_sim reaches a verdict. Everything is typed: no Envelope crosses.
 *
 *   pnpm smoke                    # nodes, status, rc, telemetry, profiles
 *   OTA_BUNDLE=/path/x.ota pnpm smoke   # plus an update of the first drone_sim
 *
 * Exits non-zero on the first failed expectation. Timings are printed.
 */

import { create } from "@bufbuild/protobuf";
import WebSocket from "ws";

import { GatewayMessageSchema, OtaCommand_Op, OtaState_Phase, ProfileCommand_Op, type GatewayMessage } from "../src/gen/gateway_pb";
import { NodeKind } from "../src/gen/mark4_pb";
import { decodeGatewayMessage, encodeGatewayMessage } from "../src/shared/gateway_socket";
import { pilotInput, SAFE_RC } from "../src/console/rc";

const URL = process.env["HUB_URL"] ?? "ws://127.0.0.1:47810";
const startedAt = Date.now();
const log = (text: string): void => console.log(`[${((Date.now() - startedAt) / 1000).toFixed(2)} s] ${text}`);
const fail = (text: string): never => {
    console.error(`FAIL: ${text}`);
    process.exit(1);
};

const ws = new WebSocket(URL);
ws.binaryType = "arraybuffer";
const inbox: GatewayMessage[] = [];
const waiters: ((message: GatewayMessage) => boolean)[] = [];
ws.on("message", (data: ArrayBuffer) => {
    const message = decodeGatewayMessage(new Uint8Array(data));
    if (message === null) {
        return fail("undecodable websocket message");
    }
    inbox.push(message);
    for (const waiter of [...waiters]) {
        if (waiter(message)) {
            waiters.splice(waiters.indexOf(waiter), 1);
        }
    }
});

let nextId = 1;
function request(message: GatewayMessage): Promise<{ ok: boolean; error: string }> {
    message.id = nextId++;
    ws.send(encodeGatewayMessage(message));
    return new Promise((resolve) =>
        waiters.push((reply) => {
            if (reply.body.case === "ack" && reply.id === message.id) {
                resolve({ ok: reply.body.value.ok, error: reply.body.value.error });
                return true;
            }
            return false;
        })
    );
}

/** Waits for a message matching a predicate, failing after the timeout. */
function waitFor<T>(what: string, pick: (message: GatewayMessage) => T | undefined, timeoutMs = 10000): Promise<T> {
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

/**
 * Same as waitFor, but a message that already arrived counts. The gateway
 * publishes some things only on change and to every client that connects,
 * so the answer may be in the inbox before the wait is even registered.
 */
function waitForPastOrNext<T>(
    what: string,
    pick: (message: GatewayMessage) => T | undefined,
    timeoutMs = 10000
): Promise<T> {
    for (const message of inbox) {
        const value = pick(message);
        if (value !== undefined) {
            return Promise.resolve(value);
        }
    }
    return waitFor(what, pick, timeoutMs);
}

/** The report of one node, when this message is one. */
function statusOf(message: GatewayMessage, node: number) {
    return message.body.case === "nodeStatus" && message.body.value.node === node
        ? message.body.value.status
        : undefined;
}

await new Promise<void>((resolve) => ws.on("open", () => resolve()));
log(`connected to ${URL}`);

const status = await waitFor("GatewayStatus", (m) => (m.body.case === "status" ? m.body.value : undefined));
log(`gateway node ${status.nodeId}, wire ${status.wireHash.toString(16)}, ${status.clients} client(s)`);

// The table must name the gateway, the plant and two distinct drone_sim,
// and stay stable across three consecutive tables (no flapping).
const tables: number[][] = [];
while (tables.length < 3) {
    const table = await waitFor("NodeTable", (m) => (m.body.case === "nodes" ? m.body.value : undefined));
    const kinds = table.nodes.map((node) => `${node.id}:${NodeKind[node.announce?.kind ?? 0]}`);
    log(`nodes: ${kinds.join(" ")}`);
    const drones = table.nodes.filter((node) => node.announce?.kind === NodeKind.DRONE_SIM).map((node) => node.id).sort();
    if (!table.nodes.some((node) => node.announce?.kind === NodeKind.GATEWAY)) fail("no gateway in the table");
    if (!table.nodes.some((node) => node.announce?.kind === NodeKind.PLANT)) fail("no plant in the table");
    if (drones.length < 2) {
        log("waiting for two drone_sim...");
        tables.length = 0;
        continue;
    }
    tables.push(drones);
}
if (tables.some((drones) => drones.join() !== tables[0]!.join())) fail(`drone ids flapped: ${JSON.stringify(tables)}`);
const droneIds = tables[0]!;
log(`two stable drone_sim nodes: ${droneIds.join(", ")}`);

// Status from both, with truth.
for (const id of droneIds) {
    const status = await waitFor(`status from ${id}`, (m) => statusOf(m, id));
    log(`status from ${id}: phase ${status.flightPhase}, truth ${status.truth ? "present" : "ABSENT"}`);
    if (status.truth === undefined) fail(`no truth in the status of ${id}`);
}

// Rc to the first drone: kill off + arm on, then watch its phase leave idle
// (armed = 2) while the other drone stays where it was.
const [pilot, bystander] = droneIds as [number, number];
const before = await waitFor(`status from ${bystander}`, (m) => statusOf(m, bystander)?.flightPhase);
const rcTimer = setInterval(
    () => ws.send(encodeGatewayMessage(pilotInput(pilot, { ...SAFE_RC, kill: false, arm: true }))),
    100
);
const armedAt = Date.now();
const armedPhase = await waitFor(`armed phase from ${pilot}`, (m) => {
    const phase = statusOf(m, pilot)?.flightPhase;
    return phase !== undefined && phase !== 0 ? phase : undefined;
});
log(`drone ${pilot} left idle (phase ${armedPhase}) ${Date.now() - armedAt} ms after the first pilot input`);
const after = await waitFor(`status from ${bystander}`, (m) => statusOf(m, bystander)?.flightPhase);
if (after !== before) fail(`the other drone changed phase too: ${before} -> ${after}`);
log(`drone ${bystander} untouched (phase ${after})`);
clearInterval(rcTimer);
// Back to safe, twice, then silence: the drone's own timeout does the rest
for (let i = 0; i < 2; ++i) ws.send(encodeGatewayMessage(pilotInput(pilot, SAFE_RC)));

// The whole telemetry chain, end to end: the gateway pulls each drone's
// descriptor table and publishes it, a configuration comes back as the
// configuration applied, the samples arrive at the period asked for, and
// they stop when the subscription is given back.
const table = await waitForPastOrNext(
    `a telemetry table for ${pilot}`,
    (m) =>
        m.body.case === "nodeTelemetry" &&
        m.body.value.node === pilot &&
        m.body.value.descriptors.length > 0
            ? m.body.value.descriptors
            : undefined
);
log(`drone ${pilot} exposes ${table.length} measures, first: ${table[0]?.name}`);
if (table.length < 3) fail(`only ${table.length} measures in the table of ${pilot}`);

const enabledIds = table.slice(0, 3).map((descriptor) => descriptor.id);
const PERIOD_MS = 50;
const telemetryCommand = (action: { case: "config"; value: { ids: number[]; periodMs: number } } | { case: "subscribe"; value: boolean }): void =>
    ws.send(
        encodeGatewayMessage(
            create(GatewayMessageSchema, { body: { case: "telemetryCommand", value: { node: pilot, action } } })
        )
    );
const subscribe = (enabled: boolean): void => telemetryCommand({ case: "subscribe", value: enabled });
// The configuration says what the stream carries, the subscription says
// who gets it: two commands, and nothing to repeat afterwards.
const enabledAt = Date.now();
telemetryCommand({ case: "config", value: { ids: enabledIds, periodMs: PERIOD_MS } });
subscribe(true);

const applied = await waitFor(`a NodeTelemetryConfig for ${pilot}`, (m) =>
    m.body.case === "nodeTelemetryConfig" && m.body.value.node === pilot ? m.body.value : undefined
);
log(`config from ${pilot}: ${applied.ids.length} measures every ${applied.periodMs} ms (${Date.now() - enabledAt} ms)`);
if (applied.ids.length !== enabledIds.length) fail(`the drone kept ${applied.ids.length} of ${enabledIds.length} ids`);
if (applied.periodMs !== PERIOD_MS) fail(`the drone applied ${applied.periodMs} ms instead of ${PERIOD_MS}`);

// A second of samples at 50 ms is 20 messages; 15 leaves room for the
// datagram that goes missing on a busy bench.
const EXPECTED_SAMPLES = 15;
let samples = 0;
let lastSampleAt = 0;
const countSamples = (message: GatewayMessage): boolean => {
    if (message.body.case !== "telemetrySamples" || message.body.value.node !== pilot) {
        return false;
    }
    const values = message.body.value.data?.values ?? [];
    if (values.length !== enabledIds.length) {
        fail(`a sample carried ${values.length} values, expected ${enabledIds.length}`);
    }
    samples += 1;
    lastSampleAt = Date.now();
    return false;
};
waiters.push(countSamples);
await new Promise<void>((resolve) => setTimeout(resolve, 1000));
log(`${samples} sample messages from ${pilot} in one second`);
if (samples < EXPECTED_SAMPLES) fail(`only ${samples} sample messages, expected ${EXPECTED_SAMPLES}`);

// Give the stream back: the drone stops emitting it at once.
subscribe(false);
const stoppedAt = Date.now();
await new Promise<void>((resolve) => setTimeout(resolve, 1000));
waiters.splice(waiters.indexOf(countSamples), 1);
const quietFor = Date.now() - lastSampleAt;
log(`the stream stopped ${lastSampleAt - stoppedAt} ms after the unsubscribe (quiet for ${quietFor} ms)`);
if (quietFor < 500) fail(`${pilot} was still streaming after the unsubscribe`);

// The profile service.
// The answer is broadcast before the ack: listen first, then ask.
const namesPending = waitFor("ProfileList", (m) => (m.body.case === "profiles" ? m.body.value.names : undefined));
const listAck = await request(create(GatewayMessageSchema, { body: { case: "profileCommand", value: { op: ProfileCommand_Op.LIST } } }));
if (!listAck.ok) fail(`profile list refused: ${listAck.error}`);
const names = await namesPending;
log(`profiles: [${names.join(", ")}]`);

// An OTA against the first drone_sim, when a bundle is given.
const bundle = process.env["OTA_BUNDLE"];
if (bundle !== undefined) {
    const otaAt = Date.now();
    const startAck = await request(
        create(GatewayMessageSchema, {
            body: { case: "otaCommand", value: { op: OtaCommand_Op.START, targetNode: pilot, bundlePath: bundle } },
        })
    );
    if (!startAck.ok) fail(`ota start refused: ${startAck.error}`);
    let lastPhase = -1;
    const finalState = await waitFor(
        "OtaState verdict",
        (m) => {
            if (m.body.case !== "otaState") return undefined;
            if (m.body.value.phase !== lastPhase) {
                lastPhase = m.body.value.phase;
                log(`ota phase ${OtaState_Phase[lastPhase]} (${m.body.value.progress?.ackedBytes ?? 0}/${m.body.value.progress?.totalBytes ?? 0} bytes)`);
            }
            return m.body.value.verdict !== 0 ? m.body.value : undefined;
        },
        120000
    );
    log(`ota verdict ${finalState.verdict} "${finalState.verdictText}" after ${Date.now() - otaAt} ms, target node ${finalState.targetNode}`);
    if (finalState.phase !== OtaState_Phase.CONFIRMED) fail(`ota did not confirm: ${finalState.lastError}`);
}

log(`done: ${inbox.length} messages received`);
ws.close();
process.exit(0);
