// The TextMate grammar of the "Mark4 Logs" channel against real lines. VS
// Code cannot run here, so what is checked is what the grammar is: the line
// pattern and the sub-pattern picking the scope, applied to formatLogLine
// outputs. Colors code the level and nothing else: one scope for the whole
// line, none at all for INFO.

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
const whole = rule.captures["1"] as Capture;

/** The scope the grammar gives to a whole line, if it gives one. */
function scopeOf(text: string): string | undefined {
    const columns = line.exec(text);
    if (columns === null) {
        return undefined;
    }
    const captured = columns[1] as string;
    assert.equal(captured, text, "the line rule captures the whole line");
    return whole.patterns?.find((sub) => new RegExp(sub.match).test(captured))?.name;
}

const SIM = 0xd5000001;
const at = new Date(2026, 7, 30, 12, 34, 56, 7);

test("the grammar is the one the package contributes", () => {
    assert.equal(grammar.scopeName, "source.mark4-log");
    assert.equal(grammar.patterns.length, 1, "one line rule: anything else could bleed into the line");
    assert.deepEqual(Object.keys(rule.captures), ["1"], "one capture: the whole line");
});

test("the level, and it alone, colors the whole line", () => {
    const expected = new Map<LogLevel, string | undefined>([
        [LogLevel.TRACE, "comment.line.mark4-log"],
        [LogLevel.DEBUG, "comment.line.mark4-log"],
        [LogLevel.INFO, undefined],
        [LogLevel.WARN, "string.unquoted.line.warn.mark4-log"],
        [LogLevel.ERROR, "invalid.illegal.line.error.mark4-log"],
    ]);
    for (const [level, scope] of expected) {
        assert.equal(scopeOf(formatLogLine(at, level, "firmware", SIM, "app/boot", "x")), scope, `level ${level}`);
    }
});

test("the kind, the node id and the module do not change the scope", () => {
    for (const kind of ["firmware", "drone_sim", "plant", "gateway", "batch", "relay", "phone", "unknown"]) {
        assert.equal(
            scopeOf(formatLogLine(at, LogLevel.WARN, kind, SIM, "app/boot", "node d5000001 left")),
            "string.unquoted.line.warn.mark4-log",
            kind,
        );
    }
    // The gateway link pseudo node and a module the table has not named yet
    // are lines like any other.
    assert.equal(scopeOf(formatLogLine(at, LogLevel.INFO, "gateway", 0, "gateway link", "reconnected")), undefined);
    assert.equal(
        scopeOf(formatLogLine(at, LogLevel.ERROR, "unknown", SIM, "#123", "early")),
        "invalid.illegal.line.error.mark4-log",
    );
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
