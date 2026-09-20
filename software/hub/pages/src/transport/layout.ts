/**
 * Where the graph puts every node.
 *
 * The reports name the media: for every reporting node and every link it
 * has, the medium of that link holds the node itself plus every peer it
 * hears directly over it. Two such instances of the same medium kind that
 * share a member are the same wire seen from two ends, so they merge. What
 * comes out is the picture of the bench: one bus per medium, the nodes
 * beside it, and the relay between a serial bus and the LAN.
 *
 * Columns go left to right: the serial-only nodes, their buses, the nodes on
 * both a serial line and the LAN, the LAN buses, the LAN-only nodes, and
 * last the nodes no report places, which the gateway only reaches through a
 * relay. A column nothing falls into takes no width.
 *
 * Pure arithmetic: no DOM, no message. The graph paints what it returns.
 */

import { LinkKind, NodeKind } from "../gen/mark4_pb";

/** Width of one column [px]. */
export const COLUMN_W = 240;
/** Width of one card [px]; the rest of its column is where the stubs run. */
export const CARD_W = 200;
/** Height of one card [px]. */
export const CARD_H = 96;
/** Vertical space between two cards [px]. */
export const ROW_GAP = 16;
/** Where the port chips of a card sit [px from its top]: the stubs leave there. */
export const PORT_Y = 78;
/** Space around the whole drawing [px]. */
export const MARGIN = 16;

const COL_UART_NODES = 0;
const COL_UART_BUSES = 1;
const COL_BOTH = 2;
const COL_UDP_BUSES = 3;
const COL_UDP_NODES = 4;
const COL_UNPLACED = 5;
const COLUMN_COUNT = 6;

/** The order the cards of one column stack in, before their ids. */
const KIND_ORDER: readonly NodeKind[] = [
    NodeKind.GATEWAY,
    NodeKind.RELAY,
    NodeKind.FIRMWARE,
    NodeKind.DRONE_SIM,
    NodeKind.PLANT,
    NodeKind.PHONE,
];

/** One peer as the reporting node holds it. */
export interface LayoutPeer {
    readonly id: number;
    /** Index into the node's links. */
    readonly link: number;
    /** Relays the last frame from it crossed; only a direct peer is on the medium. */
    readonly hops: number;
}

/** One node to place. */
export interface LayoutNode {
    readonly id: number;
    readonly kind: NodeKind;
    /** The medium of each of its links, empty when it does not report. */
    readonly links: readonly LinkKind[];
    readonly peers: readonly LayoutPeer[];
    /** Relays between the gateway and it, -1 when the gateway does not hear it. */
    readonly hops: number;
}

/** One medium: a bus and everything on it. */
export interface Medium {
    readonly kind: LinkKind;
    readonly members: readonly number[];
}

/** One node's card, placed. */
export interface Card {
    readonly id: number;
    readonly x: number;
    readonly y: number;
    /** How it was placed when no medium holds it, empty otherwise. */
    readonly note: string;
}

/** One medium's bus line. */
export interface Bus {
    /** Index into the layout's media. */
    readonly medium: number;
    readonly kind: LinkKind;
    readonly x: number;
    readonly y0: number;
    readonly y1: number;
}

/** What ties one card to one bus. */
export interface Stub {
    readonly node: number;
    /** The node's link index, -1 for a member that does not report. */
    readonly link: number;
    readonly medium: number;
    /** At the card. */
    readonly x0: number;
    /** At the bus. */
    readonly x1: number;
    readonly y: number;
}

/** The whole drawing, in pixels. */
export interface Layout {
    readonly media: readonly Medium[];
    readonly cards: readonly Card[];
    readonly buses: readonly Bus[];
    readonly stubs: readonly Stub[];
    readonly width: number;
    readonly height: number;
}

/** Places every node, its media and the lines between them. */
export function layoutGraph(nodes: readonly LayoutNode[]): Layout {
    const media = buildMedia(nodes);
    const homes = membership(media);
    const columns = assign(nodes, media, homes);
    const columnX = spread(columns);
    const cards = place(nodes, media, homes, columns, columnX);
    const cardById = new Map(cards.map((card) => [card.id, card]));
    const busX = busPositions(media, columnX);
    const stubs = connect(nodes, media, homes, cardById, busX);
    const buses = busLines(media, busX, stubs);

    let height = 0;
    for (const card of cards) {
        height = Math.max(height, card.y + CARD_H);
    }
    let width = 0;
    for (const x of columnX.values()) {
        width = Math.max(width, x + COLUMN_W);
    }
    return { media, cards, buses, stubs, width: width + MARGIN, height: height + MARGIN };
}

/** Which column a node stacks in, by node id. */
function assign(
    nodes: readonly LayoutNode[],
    media: readonly Medium[],
    homes: ReadonlyMap<number, number[]>
): Map<number, number> {
    const columns = new Map<number, number>();
    for (const node of nodes) {
        const mine = homes.get(node.id) ?? [];
        const uart = mine.some((index) => media[index]?.kind === LinkKind.LINK_UART);
        const udp = mine.some((index) => media[index]?.kind === LinkKind.LINK_UDP);
        if (uart && udp) {
            columns.set(node.id, COL_BOTH);
        } else if (uart) {
            columns.set(node.id, COL_UART_NODES);
        } else if (udp) {
            columns.set(node.id, COL_UDP_NODES);
        } else {
            columns.set(node.id, COL_UNPLACED);
        }
    }
    return columns;
}

/** The left edge of every column that holds something, in order. */
function spread(columns: ReadonlyMap<number, number>): Map<number, number> {
    const used = new Set<number>();
    for (const column of columns.values()) {
        used.add(column);
        // A column of nodes needs the bus column that serves it.
        if (column === COL_UART_NODES || column === COL_BOTH) {
            used.add(COL_UART_BUSES);
        }
        if (column === COL_UDP_NODES || column === COL_BOTH) {
            used.add(COL_UDP_BUSES);
        }
    }
    const x = new Map<number, number>();
    let next = MARGIN;
    for (let column = 0; column < COLUMN_COUNT; column++) {
        if (used.has(column)) {
            x.set(column, next);
            next += COLUMN_W;
        }
    }
    return x;
}

/** Stacks the cards of every column, the first one grouped by its medium. */
function place(
    nodes: readonly LayoutNode[],
    media: readonly Medium[],
    homes: ReadonlyMap<number, number[]>,
    columns: ReadonlyMap<number, number>,
    columnX: ReadonlyMap<number, number>
): Card[] {
    const cards: Card[] = [];
    for (const [column, left] of columnX) {
        const mine = nodes.filter((node) => columns.get(node.id) === column);
        if (mine.length === 0) {
            continue;
        }
        // The serial column reads as its buses do: one group per medium.
        const groups =
            column === COL_UART_NODES
                ? media.map((_medium, index) => mine.filter((node) => (homes.get(node.id) ?? [])[0] === index))
                : [mine];
        let y = MARGIN;
        for (const group of groups) {
            if (group.length === 0) {
                continue;
            }
            for (const node of [...group].sort(compareNodes)) {
                cards.push({ id: node.id, x: left + (COLUMN_W - CARD_W) / 2, y, note: note(node, column) });
                y += CARD_H + ROW_GAP;
            }
            y += ROW_GAP;
        }
    }
    return cards;
}

/** How a node nothing places is labelled: by the hops the gateway sees it at. */
function note(node: LayoutNode, column: number): string {
    if (column !== COL_UNPLACED) {
        return "";
    }
    return node.hops > 0 ? `via ${node.hops} hops` : "unplaced";
}

/** Gateway first, then the relay, the drones, the plant and the phone. */
function compareNodes(a: LayoutNode, b: LayoutNode): number {
    return kindRank(a.kind) - kindRank(b.kind) || a.id - b.id;
}

function kindRank(kind: NodeKind): number {
    const rank = KIND_ORDER.indexOf(kind);
    return rank < 0 ? KIND_ORDER.length : rank;
}

/** Where each medium's bus line runs, by medium index. */
function busPositions(media: readonly Medium[], columnX: ReadonlyMap<number, number>): Map<number, number> {
    const x = new Map<number, number>();
    for (const kind of [LinkKind.LINK_UART, LinkKind.LINK_UDP]) {
        const column = kind === LinkKind.LINK_UART ? COL_UART_BUSES : COL_UDP_BUSES;
        const left = columnX.get(column);
        const mine = media.map((medium, index) => ({ medium, index })).filter((one) => one.medium.kind === kind);
        if (left === undefined || mine.length === 0) {
            continue;
        }
        // Several buses share their column, evenly spaced across it.
        mine.forEach((one, rank) => {
            x.set(one.index, left + (COLUMN_W * (rank + 1)) / (mine.length + 1));
        });
    }
    return x;
}

/** One stub per card and medium it is on, from the card's ports to the bus. */
function connect(
    nodes: readonly LayoutNode[],
    media: readonly Medium[],
    homes: ReadonlyMap<number, number[]>,
    cards: ReadonlyMap<number, Card>,
    busX: ReadonlyMap<number, number>
): Stub[] {
    const stubs: Stub[] = [];
    for (const node of nodes) {
        const card = cards.get(node.id);
        if (card === undefined) {
            continue;
        }
        for (const index of homes.get(node.id) ?? []) {
            const bus = busX.get(index);
            if (bus === undefined) {
                continue;
            }
            const right = bus > card.x;
            stubs.push({
                node: node.id,
                link: linkInto(node, media[index]?.kind ?? LinkKind.LINK_KIND_UNSPECIFIED),
                medium: index,
                x0: right ? card.x + CARD_W : card.x,
                x1: bus,
                y: card.y + PORT_Y,
            });
        }
    }
    return stubs;
}

/** The link of a node that opens onto a medium kind, -1 when it reports none. */
function linkInto(node: LayoutNode, kind: LinkKind): number {
    return node.links.findIndex((one) => one === kind);
}

/** Every bus, drawn from its first stub to its last. */
function busLines(media: readonly Medium[], busX: ReadonlyMap<number, number>, stubs: readonly Stub[]): Bus[] {
    const buses: Bus[] = [];
    media.forEach((medium, index) => {
        const x = busX.get(index);
        const mine = stubs.filter((stub) => stub.medium === index);
        if (x === undefined || mine.length === 0) {
            return;
        }
        let y0 = Number.POSITIVE_INFINITY;
        let y1 = Number.NEGATIVE_INFINITY;
        for (const stub of mine) {
            y0 = Math.min(y0, stub.y);
            y1 = Math.max(y1, stub.y);
        }
        buses.push({ medium: index, kind: medium.kind, x, y0, y1 });
    });
    return buses;
}

/** Every node's media, by node id. */
function membership(media: readonly Medium[]): Map<number, number[]> {
    const homes = new Map<number, number[]>();
    media.forEach((medium, index) => {
        for (const member of medium.members) {
            const mine = homes.get(member);
            if (mine === undefined) {
                homes.set(member, [index]);
            } else {
                mine.push(index);
            }
        }
    });
    return homes;
}

/**
 * The media the reports describe: one instance per reporting node and link,
 * then the instances of one kind that share a member merged into one.
 */
function buildMedia(nodes: readonly LayoutNode[]): Medium[] {
    const kinds: LinkKind[] = [];
    const members: Set<number>[] = [];
    for (const node of nodes) {
        node.links.forEach((kind, index) => {
            const mine = new Set<number>([node.id]);
            for (const peer of node.peers) {
                if (peer.link === index && peer.hops === 0) {
                    mine.add(peer.id);
                }
            }
            kinds.push(kind);
            members.push(mine);
        });
    }

    const owner = kinds.map((_kind, index) => index);
    const root = (index: number): number => {
        let walk = index;
        while (owner[walk] !== walk) {
            walk = owner[walk] ?? walk;
        }
        return walk;
    };
    for (let a = 0; a < kinds.length; a++) {
        for (let b = a + 1; b < kinds.length; b++) {
            if (kinds[a] === kinds[b] && shares(members[a], members[b])) {
                owner[root(b)] = root(a);
            }
        }
    }

    const media: Medium[] = [];
    const slot = new Map<number, number>();
    kinds.forEach((kind, index) => {
        const home = root(index);
        const known = slot.get(home);
        const mine = members[index] ?? new Set<number>();
        if (known === undefined) {
            slot.set(home, media.length);
            media.push({ kind, members: [...mine] });
            return;
        }
        const grown = new Set([...(media[known]?.members ?? []), ...mine]);
        media[known] = { kind, members: [...grown] };
    });
    return media.map((medium) => ({ kind: medium.kind, members: [...medium.members].sort((a, b) => a - b) }));
}

/** True when two member sets have a node in common. */
function shares(a: Set<number> | undefined, b: Set<number> | undefined): boolean {
    if (a === undefined || b === undefined) {
        return false;
    }
    for (const member of a) {
        if (b.has(member)) {
            return true;
        }
    }
    return false;
}
