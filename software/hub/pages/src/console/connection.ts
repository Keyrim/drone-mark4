/**
 * The connection model of the control page, kept pure so the one rule of
 * the bench is testable: whatever the route (announced UDP process, WiFi
 * bridge, UART), the workflow is the same list of candidates, one of which
 * may be THE connected drone. A connected drone that goes silent keeps its
 * row with the "lost" state instead of vanishing: the hub holds on to the
 * connection and relights it when the same drone comes back.
 */

/** What the hub says about THE connected drone, inside the status message. */
export interface Connection {
    via: string; // "none", "udp", "uart" or "bridge"
    id: string;
    kind: number;
    kindName: string;
    live: boolean;
}

/** The no-drone state, what a fresh page assumes until the hub speaks. */
export const NO_CONNECTION: Connection = {
    via: "none",
    id: "",
    kind: 0,
    kindName: "",
    live: false,
};

/** One announced flight process, as the discovery message lists it. */
export interface AnnouncedProcess {
    kind: number;
    kindName: string;
    viaSerial: boolean;
}

/** One WiFi bridge, as the discovery message lists it. */
export interface AnnouncedBridge {
    address: string;
    port: number;
    name: string;
}

export type RowState = "connected" | "lost" | "available";

/** One row of the connection list. */
export interface CandidateRow {
    label: string;
    detail: string;
    state: RowState;
    /** The connect request this row sends, null when it offers none. */
    connect: Record<string, unknown> | null;
}

/**
 * The connection list: every announced UDP process and every bridge, plus
 * the row of a connected drone that is not announced any more (that row is
 * the memory of the connection, so it must survive the disappearance).
 * The UART door is not listed here: nothing announces a cable, so the page
 * renders it as its own manual row.
 */
export function candidateRows(
    processes: AnnouncedProcess[],
    bridges: AnnouncedBridge[],
    connection: Connection,
): CandidateRow[] {
    const rows: CandidateRow[] = [];
    let connectionListed = false;

    for (const process of processes) {
        if (process.viaSerial) {
            // Serial telemetry is the board already connected through the
            // UART or bridge row: it is evidence, not a candidate.
            continue;
        }
        const mine = connection.via === "udp" && connection.id === process.kindName;
        connectionListed ||= mine;
        rows.push({
            label: process.kindName,
            detail: "udp",
            state: mine ? "connected" : "available",
            connect: mine ? null : { type: "connect", via: "udp", target: process.kindName },
        });
    }

    for (const bridge of bridges) {
        const mine = connection.via === "bridge" && connection.id === bridge.name;
        connectionListed ||= mine;
        rows.push({
            label: bridge.name === "" ? "unnamed bridge" : bridge.name,
            detail: `${bridge.address}:${bridge.port}`,
            state: mine ? (connection.live ? "connected" : "lost") : "available",
            // A nameless bridge cannot be a connection target: the name is
            // the identity the hub reconnects on.
            connect:
                mine || bridge.name === ""
                    ? null
                    : { type: "connect", via: "bridge", name: bridge.name },
        });
    }

    if ((connection.via === "udp" || connection.via === "bridge") && !connectionListed) {
        rows.push({
            label: connection.id,
            detail: connection.via,
            state: "lost",
            connect: null,
        });
    }
    return rows;
}
