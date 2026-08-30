/**
 * Websocket link to the gateway. The gateway serves these pages and its
 * websocket endpoint on the same port, so the URL is always derived from the
 * location: no host to configure, no port to guess.
 *
 * Every message, both ways, is one binary GatewayMessage (gateway.proto).
 * Handlers are registered per body case; a case a page does not handle is
 * ignored, so a gateway that learns a new message never breaks an older page.
 * Transport frames carry one Envelope (mark4.proto): `onEnvelope` decodes it
 * and hands the sender's node id along.
 */

import { create, fromBinary, toBinary } from "@bufbuild/protobuf";

import { type GatewayMessage, GatewayMessageSchema } from "../gen/gateway_pb";
import { type Envelope, EnvelopeSchema } from "../gen/mark4_pb";

/** Answer to a request that carried a correlation id. */
export interface Ack {
    ok: boolean;
    error: string;
}

export type ConnectionState = "connecting" | "open" | "closed";

/** The oneof of GatewayMessage, minus the empty case. */
type Body = Exclude<GatewayMessage["body"], { case: undefined }>;
export type BodyCase = Body["case"];
type BodyValue<K extends BodyCase> = Extract<Body, { case: K }>["value"];
type Handler<K extends BodyCase> = (value: BodyValue<K>) => void;
type EnvelopeHandler = (src: number, envelope: Envelope) => void;

/** The subset of WebSocket the link uses, so a test can hand in a fake. */
export interface SocketLike {
    readonly readyState: number;
    binaryType: string;
    send(data: ArrayBufferLike | ArrayBufferView): void;
    close(): void;
    addEventListener(type: string, listener: (event: any) => void): void;
}

const RECONNECT_MIN_MS = 250;
const RECONNECT_MAX_MS = 5000;
const REQUEST_TIMEOUT_MS = 5000;
/** Correlation ids are numbered inside a per-tab nonce block of this size. */
const NONCE_BLOCK = 65536;
/** Nonce ceiling: nonce * NONCE_BLOCK + counter stays a uint32, and never 0. */
const NONCE_MAX = 65535;
const WS_OPEN = 1;

/** Decodes one binary websocket message, null when it is not a GatewayMessage. */
export function decodeGatewayMessage(bytes: Uint8Array): GatewayMessage | null {
    try {
        return fromBinary(GatewayMessageSchema, bytes);
    } catch {
        return null;
    }
}

/** Encodes one message, the inverse of decodeGatewayMessage. */
export function encodeGatewayMessage(message: GatewayMessage): Uint8Array {
    return toBinary(GatewayMessageSchema, message);
}

/** One Frame message carrying an Envelope for a node (0 = every node). */
export function frameMessage(dst: number, envelope: Envelope, id = 0): GatewayMessage {
    return create(GatewayMessageSchema, {
        id,
        body: { case: "frame", value: { src: 0, dst, payload: toBinary(EnvelopeSchema, envelope) } },
    });
}

export class GatewaySocket {
    private socket: SocketLike | null = null;
    private readonly handlers = new Map<BodyCase, Handler<BodyCase>[]>();
    private readonly envelopeHandlers: EnvelopeHandler[] = [];
    private readonly pending = new Map<number, (ack: Ack) => void>();
    private readonly stateHandlers: ((state: ConnectionState) => void)[] = [];
    private state: ConnectionState = "connecting";
    private delayMs = RECONNECT_MIN_MS;
    private retryTimer: ReturnType<typeof setTimeout> | null = null;
    /**
     * Acks are broadcast to every client, so an id must identify the tab that
     * asked. Each tab draws a random block and numbers its requests inside it.
     */
    private readonly nonce = 1 + Math.floor(Math.random() * NONCE_MAX);
    private counter = 0;

    /**
     * @param open opens one socket; the default is the browser WebSocket on
     *        the host that served the page, a test hands in a fake
     */
    constructor(
        private readonly open: () => SocketLike = () => {
            const socket = new WebSocket(`ws://${location.host}`);
            return socket;
        }
    ) {
        this.connect();
        if (typeof addEventListener === "function" && typeof document !== "undefined") {
            // A machine coming back from sleep or from a dead network gets its
            // link back at once instead of waiting out the backoff.
            addEventListener("online", () => this.retryNow());
            addEventListener("visibilitychange", () => {
                if (document.visibilityState === "visible") {
                    this.retryNow();
                }
            });
        }
    }

    /** Register a handler for one body case. Several may share a case. */
    on<K extends BodyCase>(kind: K, handler: Handler<K>): void {
        const list = this.handlers.get(kind);
        const erased = handler as unknown as Handler<BodyCase>;
        if (list) {
            list.push(erased);
        } else {
            this.handlers.set(kind, [erased]);
        }
    }

    /** Every transport frame, decoded: the node it came from and its Envelope. */
    onEnvelope(handler: EnvelopeHandler): void {
        this.envelopeHandlers.push(handler);
    }

    /** Forgets one envelope handler (a widget leaving with its node). */
    offEnvelope(handler: EnvelopeHandler): void {
        const index = this.envelopeHandlers.indexOf(handler);
        if (index >= 0) {
            this.envelopeHandlers.splice(index, 1);
        }
    }

    onState(handler: (state: ConnectionState) => void): void {
        this.stateHandlers.push(handler);
        handler(this.state);
    }

    connectionState(): ConnectionState {
        return this.state;
    }

    /** Fire and forget: no id, no answer expected. */
    send(message: GatewayMessage): void {
        if (this.socket && this.socket.readyState === WS_OPEN) {
            this.socket.send(encodeGatewayMessage(message));
        }
    }

    /** One Envelope to one node, fire and forget (the RC stream). */
    sendEnvelope(dst: number, envelope: Envelope): void {
        this.send(frameMessage(dst, envelope));
    }

    /**
     * Send a request and resolve on its ack. Rejects when the link is down or
     * when the gateway stays silent for REQUEST_TIMEOUT_MS.
     */
    request(message: GatewayMessage): Promise<Ack> {
        if (!this.socket || this.socket.readyState !== WS_OPEN) {
            return Promise.reject(new Error("gateway link is down"));
        }
        this.counter = (this.counter + 1) % NONCE_BLOCK;
        const id = this.nonce * NONCE_BLOCK + this.counter;
        message.id = id;
        this.socket.send(encodeGatewayMessage(message));
        return new Promise<Ack>((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error("no answer from the gateway"));
            }, REQUEST_TIMEOUT_MS);
            this.pending.set(id, (ack) => {
                clearTimeout(timer);
                resolve(ack);
            });
        });
    }

    /** One Envelope to one node, acknowledged by the gateway. */
    requestEnvelope(dst: number, envelope: Envelope): Promise<Ack> {
        return this.request(frameMessage(dst, envelope));
    }

    /** Feeds one received binary message; public so a test can drive it. */
    dispatch(bytes: Uint8Array): void {
        const message = decodeGatewayMessage(bytes);
        if (message === null || message.body.case === undefined) {
            return;
        }
        if (message.body.case === "ack") {
            const resolve = this.pending.get(message.id);
            if (resolve) {
                this.pending.delete(message.id);
                resolve({ ok: message.body.value.ok, error: message.body.value.error });
            }
            // An ack for another tab is not ours to look at
            return;
        }
        if (message.body.case === "frame" && this.envelopeHandlers.length > 0) {
            let envelope: Envelope | null = null;
            try {
                envelope = fromBinary(EnvelopeSchema, message.body.value.payload);
            } catch {
                envelope = null;
            }
            if (envelope !== null) {
                for (const handler of this.envelopeHandlers) {
                    handler(message.body.value.src, envelope);
                }
            }
        }
        for (const handler of this.handlers.get(message.body.case) ?? []) {
            handler(message.body.value);
        }
    }

    private setState(state: ConnectionState): void {
        if (state === this.state) {
            return;
        }
        this.state = state;
        for (const handler of this.stateHandlers) {
            handler(state);
        }
    }

    private connect(): void {
        const socket = this.open();
        socket.binaryType = "arraybuffer";
        this.socket = socket;
        socket.addEventListener("open", () => {
            this.delayMs = RECONNECT_MIN_MS;
            this.setState("open");
        });
        socket.addEventListener("close", () => this.onClosed(socket));
        socket.addEventListener("error", () => socket.close());
        socket.addEventListener("message", (event: { data: unknown }) => {
            if (event.data instanceof ArrayBuffer) {
                this.dispatch(new Uint8Array(event.data));
            } else if (event.data instanceof Uint8Array) {
                this.dispatch(event.data);
            }
        });
    }

    private onClosed(socket: SocketLike): void {
        if (socket !== this.socket) {
            return;
        }
        this.socket = null;
        this.setState("closed");
        for (const [, resolve] of this.pending) {
            resolve({ ok: false, error: "gateway link is down" });
        }
        this.pending.clear();
        this.scheduleRetry();
    }

    private scheduleRetry(): void {
        if (this.retryTimer !== null) {
            return;
        }
        // Jitter spreads the reconnections of several open tabs
        const wait = this.delayMs * (0.8 + Math.random() * 0.4);
        this.delayMs = Math.min(this.delayMs * 2, RECONNECT_MAX_MS);
        this.retryTimer = setTimeout(() => {
            this.retryTimer = null;
            this.setState("connecting");
            this.connect();
        }, wait);
    }

    /** Reconnect without waiting out the current backoff. */
    private retryNow(): void {
        if (this.socket !== null) {
            return;
        }
        if (this.retryTimer !== null) {
            clearTimeout(this.retryTimer);
            this.retryTimer = null;
        }
        this.delayMs = RECONNECT_MIN_MS;
        this.setState("connecting");
        this.connect();
    }
}
