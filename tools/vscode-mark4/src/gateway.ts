// The websocket client to the gateway (the hub), the only thing in the
// extension that speaks the wire. One binary GatewayMessage per message,
// both ways (gateway.proto, see software/hub/README.md); transport frames
// carry one Envelope of mark4.proto, handed to the handlers decoded.
//
// It reconnects on its own with a bounded backoff: the hub is started and
// stopped all day and the views must simply follow.

import { create, fromBinary, toBinary } from "@bufbuild/protobuf";
import WebSocket from "ws";

import {
    type GatewayMessage,
    GatewayMessageSchema,
    type GatewayStatus,
    type NodeTable,
} from "./gen/gateway_pb";
import { type Envelope, EnvelopeSchema, type LogLevel } from "./gen/mark4_pb";

/** The hub serves its pages and its websocket on one port, always this one. */
export const GATEWAY_URL = "ws://127.0.0.1:47810";

const RECONNECT_MIN_MS = 500;
const RECONNECT_MAX_MS = 5000;

export interface GatewayHandlers {
    /** The whole table, every second and on every change. */
    onNodes(table: NodeTable): void;
    onStatus(status: GatewayStatus): void;
    /** One transport frame, decoded: the node it came from and its Envelope. */
    onEnvelope(src: number, envelope: Envelope): void;
    /** Link state; `reconnected` is true from the second open on. */
    onState(open: boolean, reconnected: boolean): void;
}

export class GatewayClient {
    private socket: WebSocket | null = null;
    private timer: ReturnType<typeof setTimeout> | null = null;
    private delayMs = RECONNECT_MIN_MS;
    private opened = 0;
    private disposed = false;

    constructor(
        private readonly handlers: GatewayHandlers,
        private readonly url = GATEWAY_URL,
    ) {
        this.connect();
    }

    /** One Envelope to one node (0 = broadcast), fire and forget. */
    sendEnvelope(dst: number, envelope: Envelope): void {
        this.send(
            create(GatewayMessageSchema, {
                body: { case: "frame", value: { src: 0, dst, payload: toBinary(EnvelopeSchema, envelope) } },
            }),
        );
    }

    /** Moves one module of one node to a level. */
    setLogLevel(nodeId: number, moduleId: number, level: LogLevel): void {
        this.sendEnvelope(
            nodeId,
            create(EnvelopeSchema, { body: { case: "logControl", value: { request: { case: "set", value: { moduleId, level } } } } }),
        );
    }

    /** Asks a node to publish its module table again. */
    queryLogModules(nodeId: number): void {
        this.sendEnvelope(
            nodeId,
            create(EnvelopeSchema, { body: { case: "logControl", value: { request: { case: "query", value: true } } } }),
        );
    }

    dispose(): void {
        this.disposed = true;
        if (this.timer !== null) {
            clearTimeout(this.timer);
            this.timer = null;
        }
        this.socket?.close();
        this.socket = null;
    }

    private send(message: GatewayMessage): void {
        if (this.socket?.readyState === WebSocket.OPEN) {
            this.socket.send(toBinary(GatewayMessageSchema, message));
        }
    }

    private connect(): void {
        const socket = new WebSocket(this.url);
        socket.binaryType = "arraybuffer";
        this.socket = socket;
        socket.on("open", () => {
            this.delayMs = RECONNECT_MIN_MS;
            this.handlers.onState(true, this.opened++ > 0);
        });
        socket.on("message", (data: ArrayBuffer) => this.dispatch(new Uint8Array(data)));
        // An error is always followed by a close: only the close reconnects.
        socket.on("error", () => {});
        socket.on("close", () => {
            if (socket !== this.socket) {
                return;
            }
            this.socket = null;
            this.handlers.onState(false, false);
            this.retryLater();
        });
    }

    private retryLater(): void {
        if (this.disposed || this.timer !== null) {
            return;
        }
        const wait = this.delayMs;
        this.delayMs = Math.min(this.delayMs * 2, RECONNECT_MAX_MS);
        this.timer = setTimeout(() => {
            this.timer = null;
            this.connect();
        }, wait);
    }

    private dispatch(bytes: Uint8Array): void {
        let message: GatewayMessage;
        try {
            message = fromBinary(GatewayMessageSchema, bytes);
        } catch {
            return;
        }
        switch (message.body.case) {
            case "nodes":
                this.handlers.onNodes(message.body.value);
                return;
            case "status":
                this.handlers.onStatus(message.body.value);
                return;
            case "frame": {
                const frame = message.body.value;
                try {
                    this.handlers.onEnvelope(frame.src, fromBinary(EnvelopeSchema, frame.payload));
                } catch {
                    // A payload this build cannot decode is not the extension's business
                }
                return;
            }
            default:
                // Every other body is a gateway service the extension does not drive
                return;
        }
    }
}
