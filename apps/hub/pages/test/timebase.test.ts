import assert from "node:assert/strict";
import test from "node:test";

import {
    clampToData,
    formatDuration,
    formatTick,
    formatValue,
    lowerBound,
    niceStep,
    pan,
    span,
    tickStep,
    ticks,
    zoom,
} from "../src/lanes/timebase";

test("zoom keeps the anchor at the same relative position", () => {
    const vp = { t0: 0, t1: 10 };
    const zoomed = zoom(vp, 2.5, 2, 100);
    assert.equal(span(zoomed), 5);
    // The anchor sat a quarter into the old span, it must stay there
    assert.equal((2.5 - zoomed.t0) / span(zoomed), 0.25);
});

test("zoom clamps the span to the allowed range", () => {
    // The edges are recomputed from the anchor, so compare within an epsilon
    assert.ok(Math.abs(span(zoom({ t0: 0, t1: 10 }, 5, 1e9, 100)) - 0.001) < 1e-9);
    assert.ok(Math.abs(span(zoom({ t0: 0, t1: 10 }, 5, 1e-9, 40)) - 40) < 1e-9);
});

test("pan shifts both edges", () => {
    assert.deepEqual(pan({ t0: 1, t1: 3 }, 2), { t0: 3, t1: 5 });
});

test("clampToData keeps a sliver of the data visible", () => {
    // Scrolled far past the end: pulled back so the data edge stays in view
    assert.deepEqual(clampToData({ t0: 100, t1: 110 }, 20), { t0: 19, t1: 29 });
    // Scrolled far before the start: stopped at 0.9 span of empty margin
    assert.deepEqual(clampToData({ t0: -100, t1: -90 }, 20), { t0: -9, t1: 1 });
    // Inside the data: untouched
    assert.deepEqual(clampToData({ t0: 5, t1: 15 }, 20), { t0: 5, t1: 15 });
});

test("niceStep rounds up to 1, 2 or 5 times a power of ten", () => {
    assert.equal(niceStep(0.9), 1);
    assert.equal(niceStep(1.1), 2);
    assert.equal(niceStep(2.1), 5);
    assert.equal(niceStep(5.1), 10);
    assert.equal(niceStep(0.021), 0.05);
    assert.equal(niceStep(0), 1);
    assert.equal(niceStep(Number.NaN), 1);
});

test("ticks cover the viewport on the step grid", () => {
    const vp = { t0: 0, t1: 10 };
    const step = tickStep(vp, 900);
    assert.equal(step, 1);
    // The grid snap can yield a negative zero, which the reader never sees
    assert.deepEqual(
        ticks(vp, 900).map((t) => t + 0),
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    );
    // Degenerate inputs produce nothing rather than a runaway loop
    assert.deepEqual(ticks({ t0: 0, t1: 0 }, 900), []);
    assert.deepEqual(ticks(vp, 0), []);
});

test("ticks start on the first multiple inside the viewport", () => {
    assert.deepEqual(ticks({ t0: 2.5, t1: 6.5 }, 360), [3, 4, 5, 6]);
});

test("formatTick adapts its precision to the step", () => {
    assert.equal(formatTick(12.345, 0.01), "12.35 s");
    assert.equal(formatTick(12.345, 1), "12 s");
    assert.equal(formatTick(65, 1), "1:05");
    assert.equal(formatTick(-65, 1), "-1:05");
    // Below a second the minute form is not used, the reader wants decimals
    assert.equal(formatTick(65, 0.1), "65.0 s");
});

test("formatDuration switches to minutes past a minute", () => {
    assert.equal(formatDuration(-1), "0.0 s");
    assert.equal(formatDuration(12.34), "12.3 s");
    assert.equal(formatDuration(83.4), "1 min 23.4 s");
});

test("formatValue keeps a fixed decimal count and falls back to exponential", () => {
    assert.equal(formatValue(null), "-");
    assert.equal(formatValue(Number.NaN), "-");
    assert.equal(formatValue(1.5), "1.500");
    assert.equal(formatValue(1.23456789), "1.235");
    assert.equal(formatValue(0), "0.000");
    assert.equal(formatValue(1.5e-7), "0.000");
    assert.equal(formatValue(2.5e7), "2.500e+7");
});

test("lowerBound finds the last sample at or before x", () => {
    const t = [0, 1, 2, 3, 4];
    assert.equal(lowerBound(t, -1), -1);
    assert.equal(lowerBound(t, 0), 0);
    assert.equal(lowerBound(t, 2.5), 2);
    assert.equal(lowerBound(t, 99), 4);
    assert.equal(lowerBound([], 1), -1);
});
