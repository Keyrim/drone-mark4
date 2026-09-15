import { create } from "@bufbuild/protobuf";
import WebSocket from "ws";
import { GatewayMessageSchema, type GatewayMessage } from "../src/gen/gateway_pb";
import { decodeGatewayMessage, encodeGatewayMessage } from "../src/shared/gateway_socket";

const ws = new WebSocket("ws://127.0.0.1:47810");
ws.binaryType = "arraybuffer";
let node = 0;
let ids: number[] = [];
let data = 0;
let messages = 0;
let configs = 0;
let enableSent = false;
const t0 = Date.now();
const telemetry = (
    target: number,
    action:
        | { case: "config"; value: { ids: number[]; periodMs: number } }
        | { case: "subscribe"; value: boolean }
): GatewayMessage =>
    create(GatewayMessageSchema, { body: { case: "telemetryCommand", value: { node: target, action } } });
ws.on("message", (raw: ArrayBuffer) => {
    const m: GatewayMessage | null = decodeGatewayMessage(new Uint8Array(raw));
    if (m === null) return;
    messages++;
    if (m.body.case === "nodeTelemetry" && m.body.value.descriptors.length > 0 && !enableSent) {
        node = m.body.value.node;
        ids = m.body.value.descriptors.slice(0, 3).map((d) => d.id);
        console.log("table from", node.toString(16), m.body.value.descriptors.length, "measures; enabling", ids);
        enableSent = true;
        ws.send(encodeGatewayMessage(telemetry(node, { case: "config", value: { ids, periodMs: 100 } })));
        ws.send(encodeGatewayMessage(telemetry(node, { case: "subscribe", value: true })));
        setTimeout(() => {
            ws.send(encodeGatewayMessage(telemetry(node, { case: "subscribe", value: false })));
            console.log(`after 2 s: ${messages} messages, ${configs} configs, ${data} sample messages`);
            setTimeout(() => process.exit(0), 200);
        }, 2000);
    }
    if (m.body.case === "nodeTelemetryConfig" && m.body.value.node === node) {
        configs++;
        console.log("config", m.body.value.ids, m.body.value.periodMs, m.body.value.subscribed);
    }
    if (m.body.case === "telemetrySamples" && m.body.value.node === node) {
        data++;
        if (data === 1) console.log("first data", m.body.value.data?.values.length, "values at", Date.now() - t0, "ms");
    }
});
setTimeout(() => { console.log("timeout: no table seen; data", data); process.exit(1); }, 8000);
