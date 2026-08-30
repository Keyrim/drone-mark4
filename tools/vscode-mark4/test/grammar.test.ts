// The TextMate grammar of the "Mark4 Logs" channel against real lines. VS
// Code cannot run here, so what is checked is what the grammar is: the line
// pattern and its capture sub-patterns, applied to formatLogLine outputs.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";

import { LogLevel } from "../src/gen/mark4_pb";
import { formatLogLine } from "../src/logView";

interface Rule {
    readonly match: string;
    readonly name: string;
}
interface Capture {
    readonly name?: string;
    readonly patterns?: Rule[];
}

const here = path.dirname(fileURLToPath(import.meta.url));
const grammar = JSON.parse(
    readFileSync(path.join(here, "..", "syntaxes", "mark4-log.tmLanguage.json"), "utf8"),
) as { scopeName: string; patterns: { match: string; captures: Record<string, Capture> }[] };

const rule = grammar.patterns[0] as { match: string; captures: Record<string, Capture> };
const line = new RegExp(rule.match);

/** The scope a capture's sub-patterns give to a piece of a column. */
function scopeOf(capture: Capture, text: string): string | undefined {
    return capture.patterns?.find((sub) => new RegExp(sub.match).test(text))?.name;
}

const SIM = 0xd5000001;
const at = new Date(2026, 7, 30, 12, 34, 56, 7);

test("the grammar is the one the package contributes", () => {
    assert.equal(grammar.scopeName, "source.mark4-log");
    assert.equal(grammar.patterns.length, 1, "one line rule: anything else could bleed into the text");
});

test("the line pattern splits a line into its six columns", () => {
    const columns = line.exec(formatLogLine(at, LogLevel.INFO, "drone_sim", SIM, "sim/link", "up"));
    assert.notEqual(columns, null);
    assert.deepEqual(
        [...(columns as RegExpExecArray)].slice(1),
        ["12:34:56.007", "INFO ", "drone_sim", "d5000001", "sim/link".padEnd(24), "up"],
    );
});

test("every level lands on the level column, with its own scope", () => {
    const expected: Record<string, string> = {
        TRACE: "comment.line.level.mark4-log",
        DEBUG: "comment.line.level.mark4-log",
        "INFO ": "keyword.other.level.info.mark4-log",
        "WARN ": "string.unquoted.level.warn.mark4-log",
        ERROR: "invalid.illegal.level.error.mark4-log",
    };
    for (const level of [LogLevel.TRACE, LogLevel.DEBUG, LogLevel.INFO, LogLevel.WARN, LogLevel.ERROR]) {
        const columns = line.exec(formatLogLine(at, level, "firmware", SIM, "app/boot", "x"));
        assert.notEqual(columns, null, `level ${level}`);
        const column = (columns as RegExpExecArray)[2] as string;
        assert.equal(column.length, 5, "the level column is 5 characters wide");
        assert.equal(scopeOf(rule.captures["2"] as Capture, column), expected[column]);
    }
});

test("every kind lands on the kind column, and no two kinds read alike", () => {
    const kinds = ["firmware", "drone_sim", "plant", "gateway", "batch", "relay", "unknown"];
    const scopes = new Set<string>();
    for (const kind of kinds) {
        const columns = line.exec(formatLogLine(at, LogLevel.WARN, kind, SIM, "app/boot", "x"));
        assert.notEqual(columns, null, kind);
        const column = (columns as RegExpExecArray)[3] as string;
        assert.equal(column.trimEnd(), kind);
        const scope = scopeOf(rule.captures["3"] as Capture, column);
        assert.equal(typeof scope, "string", `${kind} has a scope`);
        scopes.add(scope as string);
    }
    assert.equal(scopes.size, kinds.length, "one scope per kind");
});

test("the gateway link pseudo node reads like any gateway line", () => {
    const columns = line.exec(formatLogLine(at, LogLevel.INFO, "gateway", 0, "gateway link", "reconnected"));
    assert.notEqual(columns, null);
    assert.equal((columns as RegExpExecArray)[4], "00000000");
    assert.equal((columns as RegExpExecArray)[5]?.trimEnd(), "gateway link");
    assert.equal(scopeOf(rule.captures["3"] as Capture, (columns as RegExpExecArray)[3] as string), "keyword.control.kind.gateway.mark4-log");
});

test("a module the table has not named yet still holds its column", () => {
    const columns = line.exec(formatLogLine(at, LogLevel.INFO, "unknown", SIM, "#123", "early"));
    assert.notEqual(columns, null);
    assert.equal((columns as RegExpExecArray)[5]?.trimEnd(), "#123");
    assert.equal((columns as RegExpExecArray)[6], "early");
});

test("a node id quoted in the text reads like a node id", () => {
    const columns = line.exec(formatLogLine(at, LogLevel.INFO, "gateway", 10, "gateway/ws", "node d5000001 left after 250 ms"));
    const text = (columns as RegExpExecArray)[6] as string;
    const hex = new RegExp((rule.captures["6"] as Capture).patterns?.[0]?.match as string);
    assert.equal(hex.exec(text)?.[0], "d5000001");
    assert.equal(hex.test("node 250 ms"), false, "numbers with units are left alone");
});

test("a line that does not hold the layout is left alone", () => {
    for (const text of [
        "",
        "a plain line of text",
        "12:34:56.007  INFO   drone_sim  d5000001  short  up",
        "12:34:56.007  LOUD   drone_sim  d5000001  sim/link                  up",
        "12:34:56  INFO   drone_sim  d5000001  sim/link                  up",
    ]) {
        assert.equal(line.exec(text), null, JSON.stringify(text));
    }
    // A kind wider than its column shifts everything: nothing is colored
    // rather than the wrong thing.
    assert.equal(line.exec(formatLogLine(at, LogLevel.INFO, "kind 12345", SIM, "m", "x")), null);
});
