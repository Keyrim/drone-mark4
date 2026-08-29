/**
 * The connection model of the control page, kept pure so the one rule of
 * the bench is testable: every announced node (a desktop flight process,
 * the board through its relay) is one candidate of the same list, one of
 * which may be THE connected drone. A connected drone that goes silent
 * keeps its row with the "lost" state instead of vanishing: the hub holds
 * on to the connection and relights it when the same drone comes back.
 */

/** What the hub says about THE connected drone, inside the status message. */
export interface Connection {
    via: string; // "none" or "udp"
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
}

/** Node kinds that are drones: the only ones a connection can target. */
const DRONE_KINDS = new Set(["firmware", "drone_sim"]);

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
 * The connection list: every announced process, plus the row of a
 * connected drone that is not announced any more (that row is the memory
 * of the connection, so it must survive the disappearance).
 */
export function candidateRows(
    processes: AnnouncedProcess[],
    connection: Connection,
): CandidateRow[] {
    const rows: CandidateRow[] = [];
    let connectionListed = false;

    for (const process of processes) {
        if (!DRONE_KINDS.has(process.kindName)) {
            // A plant, a gateway or a campaign is on the LAN but is not a
            // drone: nothing to connect to.
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

    if (connection.via === "udp" && !connectionListed) {
        rows.push({
            label: connection.id,
            detail: connection.via,
            state: "lost",
            connect: null,
        });
    }
    return rows;
}
