// What the extension received, before anything is decided about showing it:
// one raw record per Log envelope, in a ring so a long bench does not grow
// without end. Nothing here is formatted and nothing here is filtered; the
// timestamp is the extension's own clock, stamped when the frame arrived.

import { type LogLevel } from "./gen/mark4_pb";

/** One log line as it was received, with nothing resolved yet. */
export interface LogRecord {
    readonly receivedAt: Date;
    readonly nodeId: number;
    readonly moduleId: number;
    readonly level: LogLevel;
    readonly text: string;
}

/** Lines kept before the oldest is dropped. */
export const LOG_CAPACITY = 50000;

export class LogStore {
    private readonly items: LogRecord[] = [];
    /** Index of the oldest record once the ring is full. */
    private oldest = 0;

    constructor(private readonly capacity: number = LOG_CAPACITY) {}

    push(record: LogRecord): void {
        if (this.items.length < this.capacity) {
            this.items.push(record);
            return;
        }
        this.items[this.oldest] = record;
        this.oldest = (this.oldest + 1) % this.capacity;
    }

    /** Every record kept, oldest first. */
    records(): LogRecord[] {
        return this.items.length < this.capacity
            ? [...this.items]
            : [...this.items.slice(this.oldest), ...this.items.slice(0, this.oldest)];
    }

    clear(): void {
        this.items.length = 0;
        this.oldest = 0;
    }

    get size(): number {
        return this.items.length;
    }
}
