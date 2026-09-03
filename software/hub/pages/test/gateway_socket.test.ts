import assert from "node:assert/strict";
import test from "node:test";

import { create, toBinary } from "@bufbuild/protobuf";

import { GatewayMessageSchema } from "../src/gen/gateway_pb";
import { EnvelopeSchema, NodeKind } from "../src/gen/mark4_pb";
import {
    GatewaySocket,
    type SocketLike,
    decodeGatewayMessage,
    encodeGatewayMessage,
    frameMessage,
} from "../src/shared/gateway_socket";

/** A websocket that records what was sent and lets the test push messages in. */
class FakeSocket implements SocketLike {
    readyState = 0;
    binaryType = "blob";
    readonly sent: Uint8Array[] = [];
    private readonly listeners = new Map<string, ((event: any) => void)[]>();

    send(data: ArrayBufferLike | ArrayBufferView): void {
        this.sent.push(data instanceof Uint8Array ? data : new Uint8Array(data as ArrayBufferLike));
    }

    close(): void {
        this.readyState = 3;
        this.emit("close", {});
    }

    addEventListener(type: string, listener: (event: any) => void): void {
        const list = this.listeners.get(type) ?? [];
        list.push(listener);
        this.listeners.set(type, list);
    }

    open(): void {
        this.readyState = 1;
        this.emit("open", {});
    }

    /** Delivers one binary message the way a browser does, as an ArrayBuffer. */
    receive(bytes: Uint8Array): void {
        const copy = new Uint8Array(bytes.length);
        copy.set(bytes);
        this.emit("message", { data: copy.buffer });
    }

    private emit(type: string, event: unknown): void {
        for (const listener of this.listeners.get(type) ?? []) {
            listener(event);
        }
    }
}

test("a GatewayMessage round trips through the binary framing", () => {
    const envelope = create(EnvelopeSchema, { body: { case: "rc", value: { arm: true, throttle: 0.25 } } });
    const bytes = encodeGatewayMessage(frameMessage(9, envelope, 77));
    const back = decodeGatewayMessage(bytes);
    assert.ok(back !== null);
    assert.equal(back.id, 77);
    assert.equal(back.body.case, "frame");
    if (back.body.case === "frame") {
        assert.equal(back.body.value.dst, 9);
        assert.deepEqual(back.body.value.payload, toBinary(EnvelopeSchema, envelope));
    }
    assert.equal(decodeGatewayMessage(new Uint8Array([0xff, 0xff, 0xff])), null);
});

test("frames are decoded into envelopes with their source, other bodies dispatch by case", () => {
    const fake = new FakeSocket();
    const socket = new GatewaySocket(() => fake);
    fake.open();
    assert.equal(fake.binaryType, "arraybuffer");
    assert.equal(socket.connectionState(), "open");

    const envelopes: [number, string][] = [];
    socket.onEnvelope((src, envelope) => envelopes.push([src, envelope.body.case ?? "none"]));
    const tables: number[] = [];
    socket.on("nodes", (table) => tables.push(table.nodes.length));

    const status = create(EnvelopeSchema, { body: { case: "status", value: { throwCount: 1 } } });
    fake.receive(
        encodeGatewayMessage(
            create(GatewayMessageSchema, {
                body: { case: "frame", value: { src: 42, dst: 0, payload: toBinary(EnvelopeSchema, status) } },
            })
        )
    );
    fake.receive(
        encodeGatewayMessage(
            create(GatewayMessageSchema, {
                body: { case: "nodes", value: { nodes: [{ id: 1, announce: { kind: NodeKind.GATEWAY } }] } },
            })
        )
    );
    // Garbage is ignored, not thrown
    fake.receive(new Uint8Array([1, 2, 3, 4, 5]));
    assert.deepEqual(envelopes, [[42, "status"]]);
    assert.deepEqual(tables, [1]);
});

test("a request carries a correlation id and resolves on the ack echoing it", async () => {
    const fake = new FakeSocket();
    const socket = new GatewaySocket(() => fake);
    fake.open();

    const envelope = create(EnvelopeSchema, { body: { case: "reboot", value: {} } });
    const pending = socket.requestEnvelope(5, envelope);
    assert.equal(fake.sent.length, 1);
    const sent = decodeGatewayMessage(fake.sent[0]!)!;
    assert.notEqual(sent.id, 0);
    assert.equal(sent.body.case, "frame");

    // An ack for another tab is not ours
    fake.receive(
        encodeGatewayMessage(
            create(GatewayMessageSchema, { id: sent.id + 1, body: { case: "ack", value: { ok: true } } })
        )
    );
    fake.receive(
        encodeGatewayMessage(
            create(GatewayMessageSchema, {
                id: sent.id,
                body: { case: "ack", value: { ok: false, error: "node 5 is not reachable" } },
            })
        )
    );
    const ack = await pending;
    assert.equal(ack.ok, false);
    assert.equal(ack.error, "node 5 is not reachable");

    // Fire and forget sends without an id
    socket.sendEnvelope(5, envelope);
    assert.equal(decodeGatewayMessage(fake.sent[1]!)!.id, 0);
});

test("a closed link fails the pending requests and refuses new ones", async () => {
    const fake = new FakeSocket();
    const socket = new GatewaySocket(() => fake);
    fake.open();
    const pending = socket.request(create(GatewayMessageSchema, { body: { case: "profileCommand", value: {} } }));
    fake.close();
    assert.equal(socket.connectionState(), "closed");
    const ack = await pending;
    assert.equal(ack.ok, false);
    await assert.rejects(socket.request(create(GatewayMessageSchema, { body: { case: "profileCommand", value: {} } })));
});
