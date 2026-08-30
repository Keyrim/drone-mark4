/**
 * Bench smoke of the gateway contract, from a script instead of a browser:
 * connects to a running hub over `ws`, and with a plant and flight processes
 * on the LAN checks that the node table lists them by id, that telemetry
 * frames arrive from every drone (truth included), that an Rc frame to one
 * drone shows up in its telemetry, that the profile service answers, and
 * optionally that an OTA start against a drone_sim reaches a verdict.
 *
 *   pnpm smoke                    # nodes, telemetry, rc, profiles
 *   OTA_BUNDLE=/path/x.ota pnpm smoke   # plus an update of the first drone_sim
 *
 * Exits non-zero on the first failed expectation. Timings are printed.
 */

import { create, fromBinary } from "@bufbuild/protobuf";
import WebSocket from "ws";

import { GatewayMessageSchema, OtaCommand_Op, OtaState_Phase, ProfileCommand_Op, type GatewayMessage } from "../src/gen/gateway_pb";
import { EnvelopeSchema, NodeKind, type Envelope } from "../src/gen/mark4_pb";
import { decodeGatewayMessage, encodeGatewayMessage, frameMessage } from "../src/shared/gateway_socket";
import { rcEnvelope, SAFE_RC } from "../src/console/rc";

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

function envelopeOf(message: GatewayMessage): { src: number; envelope: Envelope } | undefined {
    if (message.body.case !== "frame") {
        return undefined;
    }
    return { src: message.body.value.src, envelope: fromBinary(EnvelopeSchema, message.body.value.payload) };
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

// Telemetry from both, with truth.
for (const id of droneIds) {
    const telemetry = await waitFor(`telemetry from ${id}`, (m) => {
        const frame = envelopeOf(m);
        return frame?.src === id && frame.envelope.body.case === "telemetry" ? frame.envelope.body.value : undefined;
    });
    log(`telemetry from ${id}: phase ${telemetry.flightPhase}, truth ${telemetry.truth ? "present" : "ABSENT"}`);
    if (telemetry.truth === undefined) fail(`no truth in the telemetry of ${id}`);
}

// Rc to the first drone: kill off + arm on, then watch its phase leave idle
// (armed = 2) while the other drone stays where it was.
const [pilot, bystander] = droneIds as [number, number];
const before = await waitFor(`telemetry from ${bystander}`, (m) => {
    const frame = envelopeOf(m);
    return frame?.src === bystander && frame.envelope.body.case === "telemetry" ? frame.envelope.body.value.flightPhase : undefined;
});
const rcTimer = setInterval(() => ws.send(encodeGatewayMessage(frameMessage(pilot, rcEnvelope({ ...SAFE_RC, kill: false, arm: true })))), 100);
const armedAt = Date.now();
const armedPhase = await waitFor(`armed phase from ${pilot}`, (m) => {
    const frame = envelopeOf(m);
    if (frame?.src !== pilot || frame.envelope.body.case !== "telemetry") return undefined;
    const phase = frame.envelope.body.value.flightPhase;
    return phase !== 0 ? phase : undefined;
});
log(`drone ${pilot} left idle (phase ${armedPhase}) ${Date.now() - armedAt} ms after the first Rc frame`);
const after = await waitFor(`telemetry from ${bystander}`, (m) => {
    const frame = envelopeOf(m);
    return frame?.src === bystander && frame.envelope.body.case === "telemetry" ? frame.envelope.body.value.flightPhase : undefined;
});
if (after !== before) fail(`the other drone changed phase too: ${before} -> ${after}`);
log(`drone ${bystander} untouched (phase ${after})`);
clearInterval(rcTimer);
// Back to safe, twice, then silence: the drone's own timeout does the rest
for (let i = 0; i < 2; ++i) ws.send(encodeGatewayMessage(frameMessage(pilot, rcEnvelope(SAFE_RC))));

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
