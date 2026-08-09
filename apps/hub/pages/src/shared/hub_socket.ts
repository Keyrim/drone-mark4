/**
 * Websocket link to the hub. The hub serves these pages and its websocket
 * endpoint on the same port, so the URL is always derived from the location:
 * no host to configure, no port to guess.
 *
 * Every hub message is a JSON object carrying a "type" field. Handlers are
 * registered per type and unknown types are ignored, so a hub that learns a
 * new message never breaks an older page.
 */

/** Any decoded hub message; only the type field is guaranteed. */
export interface HubMessage {
    type: string;
    [key: string]: unknown;
}

/** Answer to a request that carried a correlation id. */
export interface Ack {
    id: number;
    ok: boolean;
    error: string;
}

export type ConnectionState = "connecting" | "open" | "closed";

type Handler = (message: HubMessage) => void;

const RECONNECT_MIN_MS = 250;
const RECONNECT_MAX_MS = 5000;
const REQUEST_TIMEOUT_MS = 5000;
/** Correlation ids are numbered inside a per-tab nonce block of this size. */
const NONCE_BLOCK = 65536;

export class HubSocket {
    private socket: WebSocket | null = null;
    private readonly handlers = new Map<string, Handler[]>();
    private readonly pending = new Map<number, (ack: Ack) => void>();
    private readonly stateHandlers: ((state: ConnectionState) => void)[] = [];
    private state: ConnectionState = "connecting";
    private delayMs = RECONNECT_MIN_MS;
    private retryTimer: ReturnType<typeof setTimeout> | null = null;
    /**
     * Acks are broadcast to every client, so an id must identify the tab that
     * asked. Each tab draws a random block and numbers its requests inside it.
     */
    private readonly nonce = Math.floor(Math.random() * NONCE_BLOCK);
    private counter = 0;

    constructor(private readonly url = `ws://${location.host}`) {
        this.connect();
        // A machine coming back from sleep or from a dead network gets its
        // link back at once instead of waiting out the backoff.
        addEventListener("online", () => this.retryNow());
        addEventListener("visibilitychange", () => {
            if (document.visibilityState === "visible") {
                this.retryNow();
            }
        });
    }

    /** Register a handler for one message type. Several may share a type. */
    on(type: string, handler: Handler): void {
        const list = this.handlers.get(type);
        if (list) {
            list.push(handler);
        } else {
            this.handlers.set(type, [handler]);
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
    send(payload: Record<string, unknown>): void {
        if (this.socket && this.socket.readyState === WebSocket.OPEN) {
            this.socket.send(JSON.stringify(payload));
        }
    }

    /**
     * Send a request and resolve on its ack. Rejects when the link is down or
     * when the hub stays silent for REQUEST_TIMEOUT_MS.
     */
    request(payload: Record<string, unknown>): Promise<Ack> {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
            return Promise.reject(new Error("hub link is down"));
        }
        const id = this.nonce * NONCE_BLOCK + this.counter;
        this.counter += 1;
        this.socket.send(JSON.stringify({ ...payload, id }));
        return new Promise<Ack>((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error("no answer from the hub"));
            }, REQUEST_TIMEOUT_MS);
            this.pending.set(id, (ack) => {
                clearTimeout(timer);
                resolve(ack);
            });
        });
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
        const socket = new WebSocket(this.url);
        this.socket = socket;
        socket.addEventListener("open", () => {
            this.delayMs = RECONNECT_MIN_MS;
            this.setState("open");
        });
        socket.addEventListener("close", () => this.onClosed(socket));
        socket.addEventListener("error", () => socket.close());
        socket.addEventListener("message", (event: MessageEvent) => {
            if (typeof event.data === "string") {
                this.dispatch(event.data);
            }
        });
    }

    private onClosed(socket: WebSocket): void {
        if (socket !== this.socket) {
            return;
        }
        this.socket = null;
        this.setState("closed");
        for (const [, resolve] of this.pending) {
            resolve({ id: -1, ok: false, error: "hub link is down" });
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

    private dispatch(text: string): void {
        let message: HubMessage;
        try {
            message = JSON.parse(text) as HubMessage;
        } catch {
            return;
        }
        if (message === null || typeof message.type !== "string") {
            return;
        }
        if (message.type === "ack") {
            const resolve = this.pending.get(message["id"] as number);
            if (resolve) {
                this.pending.delete(message["id"] as number);
                resolve({
                    id: message["id"] as number,
                    ok: message["ok"] === true,
                    error: String(message["error"] ?? ""),
                });
            }
            // An ack for another tab is not ours to look at
            return;
        }
        for (const handler of this.handlers.get(message.type) ?? []) {
            handler(message);
        }
    }
}
