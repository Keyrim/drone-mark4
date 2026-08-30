/**
 * Control page: one widget per live drone node, the firmware panel, and the
 * 3D view.
 *
 * The node table is the world: a drone (the board through its relay, a
 * desktop flight process) gets a widget the moment the gateway lists it and
 * loses it when the gateway forgets it, transmitter parked safe on the way
 * out. Nothing is connected to, nothing is picked: every widget commands
 * the node id it was built for.
 *
 * Every command is a GatewayMessage with a correlation id; the answer is an
 * Ack that comes back to the toast strip. Nothing here reaches a UDP socket:
 * the gateway forwards the frames.
 */

import { GatewaySocket } from "../shared/gateway_socket";
import { isDrone } from "../shared/nodes";
import { Shell } from "../shared/shell";
import { AttitudePanel } from "./attitude_panel";
import { DroneWidget, type WidgetHooks } from "./drone_widget";
import { OtaPanel } from "./ota_panel";

const socket = new GatewaySocket();
const shell = new Shell(socket);

const hooks: WidgetHooks = {
    notify: (text, ok) => shell.notify(text, ok),
    ask: (message, what) => shell.ask(message, what),
};

/* -------------------- the drones -------------------- */

const droneList = document.createElement("div");
droneList.className = "drone-list";
const empty = document.createElement("span");
empty.className = "panel-note";
empty.textContent = "no drone on the network: start a flight process or power the board";
droneList.appendChild(empty);

const attitude = new AttitudePanel(socket);
const widgets = new Map<number, DroneWidget>();

shell.nodes.onChange((nodes, diff) => {
    for (const id of diff.removed) {
        widgets.get(id)?.destroy();
        widgets.delete(id);
    }
    for (const node of diff.added) {
        if (isDrone(node) && !widgets.has(node.id)) {
            const widget = new DroneWidget(socket, node, hooks);
            widgets.set(node.id, widget);
            droneList.appendChild(widget.root);
        }
    }
    for (const node of nodes) {
        widgets.get(node.id)?.update(node);
    }
    if (diff.added.length > 0 || diff.removed.length > 0) {
        empty.hidden = widgets.size > 0;
        attitude.setActive(new Map([...widgets.values()].map((widget) => [widget.nodeId, widget.node.name])));
    }
});

socket.onEnvelope((src, envelope) => {
    if (envelope.body.case === "telemetry") {
        widgets.get(src)?.onTelemetry(envelope.body.value);
    }
});

/* -------------------- firmware update -------------------- */

// Reflashing is an operation on a node of the table, not a way to add one,
// so it sits below the drone list rather than inside a widget: it has to
// stay readable while the board is rebooting and its widget has momentarily
// disappeared.
const update = new OtaPanel(socket, shell.nodes, (text, ok) => shell.notify(text, ok));

/* -------------------- layout -------------------- */

shell.content.className = "content console";
const left = document.createElement("div");
left.className = "console-col";
left.appendChild(droneList);
left.appendChild(update.root);
const right = document.createElement("div");
right.className = "console-col observe";
right.appendChild(attitude.root);
shell.content.appendChild(left);
shell.content.appendChild(right);
