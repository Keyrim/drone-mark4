import { create, fromBinary } from "@bufbuild/protobuf";
import WebSocket from "ws";
import { type GatewayMessage } from "../src/gen/gateway_pb";
import { EnvelopeSchema } from "../src/gen/mark4_pb";
import { decodeGatewayMessage, encodeGatewayMessage, frameMessage } from "../src/shared/gateway_socket";

const ws = new WebSocket("ws://127.0.0.1:47810");
ws.binaryType = "arraybuffer";
let node = 0;
let ids: number[] = [];
let data = 0;
let frames = 0;
let acks = 0;
let enableSent = false;
const t0 = Date.now();
ws.on("message", (raw: ArrayBuffer) => {
    const m: GatewayMessage | null = decodeGatewayMessage(new Uint8Array(raw));
    if (m === null) return;
    if (m.body.case === "nodeTelemetry" && m.body.value.descriptors.length > 0 && !enableSent) {
        node = m.body.value.node;
        ids = m.body.value.descriptors.slice(0, 3).map((d) => d.id);
        console.log("table from", node.toString(16), m.body.value.descriptors.length, "measures; enabling", ids);
        enableSent = true;
        const env = create(EnvelopeSchema, { body: { case: "telemetryEnable", value: { ids, periodMs: 100 } } });
        ws.send(encodeGatewayMessage(frameMessage(node, env)));
        setTimeout(() => {
            const stop = create(EnvelopeSchema, { body: { case: "telemetryEnable", value: { ids: [], periodMs: 0 } } });
            ws.send(encodeGatewayMessage(frameMessage(node, stop)));
            console.log(`after 2 s: ${frames} frames, ${acks} acks, ${data} telemetryData frames`);
            setTimeout(() => process.exit(0), 200);
        }, 2000);
    }
    if (m.body.case === "frame") {
        const f = m.body.value;
        // decode envelope
        frames++;
        try {
            const env = fromBinary(EnvelopeSchema, f.payload);
            if (env.body.case === "telemetryAck") { acks++; console.log("ack", env.body.value); }
            if (env.body.case === "telemetryData" && f.src === node) { data++; if (data === 1) console.log("first data", env.body.value.values.length, "values at", Date.now() - t0, "ms"); }
        } catch (e) { console.log("decode error", String(e)); }
    }
});
setTimeout(() => { console.log("timeout: no table seen; data", data); process.exit(1); }, 8000);
