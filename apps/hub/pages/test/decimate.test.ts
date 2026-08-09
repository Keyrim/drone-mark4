import assert from "node:assert/strict";
import test from "node:test";

import { decimateMinMax } from "../src/lanes/decimate";

function ramp(n: number): [number[], (number | null)[]] {
    const t: number[] = [];
    const v: (number | null)[] = [];
    for (let i = 0; i < n; i++) {
        t.push(i);
        v.push(i);
    }
    return [t, v];
}

test("decimateMinMax leaves a series that already fits alone", () => {
    const [t, v] = ramp(100);
    const [outT, outV] = decimateMinMax(t, v, 4000);
    assert.equal(outT, t);
    assert.equal(outV, v);
});

test("decimateMinMax stays under the budget and keeps x ascending", () => {
    const [t, v] = ramp(50000);
    const [outT, outV] = decimateMinMax(t, v, 4000);
    assert.ok(outT.length <= 4000, `length ${outT.length}`);
    assert.equal(outT.length, outV.length);
    for (let i = 1; i < outT.length; i++) {
        assert.ok((outT[i] as number) > (outT[i - 1] as number), `not ascending at ${i}`);
    }
});

test("decimateMinMax keeps the envelope of a spike", () => {
    const [t, v] = ramp(20000);
    v.fill(0);
    v[12345] = 99;
    v[12346] = -99;
    const [outT, outV] = decimateMinMax(t, v, 100);
    assert.ok(outV.includes(99), "the peak was dropped");
    assert.ok(outV.includes(-99), "the trough was dropped");
    // Both extremes come out in their original order
    assert.ok(outT.indexOf(12345) < outT.indexOf(12346));
});

test("decimateMinMax keeps a hole so the line still breaks", () => {
    const [t, v] = ramp(20000);
    for (let i = 5000; i < 5200; i++) {
        v[i] = null;
    }
    const [outT, outV] = decimateMinMax(t, v, 100);
    assert.ok(outV.includes(null), "the gap was filled in");
    for (let i = 1; i < outT.length; i++) {
        assert.ok((outT[i] as number) > (outT[i - 1] as number));
    }
});

test("decimateMinMax survives a series that is only holes", () => {
    const [t, v] = ramp(20000);
    v.fill(null);
    const [outT, outV] = decimateMinMax(t, v, 100);
    assert.ok(outT.length > 0);
    assert.ok(outV.every((value) => value === null));
});

test("decimateMinMax refuses a budget too small to hold a bucket", () => {
    const [t, v] = ramp(1000);
    const [outT] = decimateMinMax(t, v, 3);
    assert.equal(outT, t);
});
