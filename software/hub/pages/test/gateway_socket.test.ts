import assert from "node:assert/strict";
import test from "node:test";

import { create } from "@bufbuild/protobuf";

import { GatewayMessageSchema, type NodeStatus } from "../src/gen/gateway_pb";
import { NodeKind } from "../src/gen/mark4_pb";
import {
    GatewaySocket,
    type SocketLike,
    decodeGatewayMessage,
    encodeGatewayMessage,
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
    const command = create(GatewayMessageSchema, {
        id: 77,
        body: {
            case: "pilotInput",
            value: { node: 9, rc: { arm: true, throttle: 0.25 } },
        },
    });
    const back = decodeGatewayMessage(encodeGatewayMessage(command));
    assert.ok(back !== null);
    assert.equal(back.id, 77);
    assert.equal(back.body.case, "pilotInput");
    if (back.body.case === "pilotInput") {
        assert.equal(back.body.value.node, 9);
        assert.equal(back.body.value.rc?.arm, true);
        assert.equal(back.body.value.rc?.throttle, 0.25);
    }
    assert.equal(decodeGatewayMessage(new Uint8Array([0xff, 0xff, 0xff])), null);
});

test("every body dispatches by case, and a handler can be forgotten", () => {
    const fake = new FakeSocket();
    const socket = new GatewaySocket(() => fake);
    fake.open();
    assert.equal(fake.binaryType, "arraybuffer");
    assert.equal(socket.connectionState(), "open");

    const reports: [number, number][] = [];
    const onStatus = (report: NodeStatus): void =>
        void reports.push([report.node, report.status?.throwCount ?? -1]);
    socket.on("nodeStatus", onStatus);
    const tables: number[] = [];
    socket.on("nodes", (table) => tables.push(table.nodes.length));

    const report = encodeGatewayMessage(
        create(GatewayMessageSchema, {
            body: { case: "nodeStatus", value: { node: 42, status: { throwCount: 1 } } },
        })
    );
    fake.receive(report);
    fake.receive(
        encodeGatewayMessage(
            create(GatewayMessageSchema, {
                body: { case: "nodes", value: { nodes: [{ id: 1, announce: { kind: NodeKind.GATEWAY } }] } },
            })
        )
    );
    // Garbage is ignored, not thrown
    fake.receive(new Uint8Array([1, 2, 3, 4, 5]));
    assert.deepEqual(reports, [[42, 1]]);
    assert.deepEqual(tables, [1]);

    // A widget leaving with its node stops hearing about it
    socket.off("nodeStatus", onStatus);
    fake.receive(report);
    assert.deepEqual(reports, [[42, 1]]);
});

test("a request carries a correlation id and resolves on the ack echoing it", async () => {
    const fake = new FakeSocket();
    const socket = new GatewaySocket(() => fake);
    fake.open();

    const reboot = create(GatewayMessageSchema, {
        body: { case: "nodeCommand", value: { node: 5, action: { case: "reboot", value: true } } },
    });
    const pending = socket.request(reboot);
    assert.equal(fake.sent.length, 1);
    const sent = decodeGatewayMessage(fake.sent[0]!)!;
    assert.notEqual(sent.id, 0);
    assert.equal(sent.body.case, "nodeCommand");

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
    socket.send(
        create(GatewayMessageSchema, { body: { case: "pilotInput", value: { node: 5, rc: {} } } })
    );
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
