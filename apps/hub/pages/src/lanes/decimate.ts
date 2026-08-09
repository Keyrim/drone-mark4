/**
 * Min/max decimation. A screen is at most a couple of thousand pixels wide,
 * so drawing every sample of a long buffer costs a lot and shows nothing
 * more. Keeping the extremes of each bucket preserves the envelope, which is
 * what matters on a gyro trace: a spike stays a spike.
 */

/** Sample count above which a series is decimated before being drawn. */
export const MAX_RENDER_POINTS = 4000;

export type Column = (number | null)[];

/**
 * Reduce (t, v) to about maxPoints samples, keeping the smallest and the
 * largest value of every bucket in their original time order, plus one null
 * per bucket that holds a hole so the line still breaks there.
 *
 * Returns the inputs untouched when they already fit.
 */
export function decimateMinMax(
    t: number[],
    v: Column,
    maxPoints = MAX_RENDER_POINTS
): [number[], Column] {
    const n = t.length;
    if (n <= maxPoints || maxPoints < 4) {
        return [t, v];
    }
    // Two samples per bucket, since each bucket yields its min and its max
    const buckets = Math.floor(maxPoints / 2);
    const outT: number[] = [];
    const outV: Column = [];

    for (let b = 0; b < buckets; b++) {
        const start = Math.floor((b * n) / buckets);
        const end = Math.floor(((b + 1) * n) / buckets);
        if (end <= start) {
            continue;
        }
        let iMin = -1;
        let iMax = -1;
        let iNull = -1;
        for (let i = start; i < end; i++) {
            const value = v[i];
            if (value === null || value === undefined || !Number.isFinite(value)) {
                iNull = i;
                continue;
            }
            if (iMin < 0 || value < (v[iMin] as number)) {
                iMin = i;
            }
            if (iMax < 0 || value > (v[iMax] as number)) {
                iMax = i;
            }
        }
        // Ascending and deduplicated: uPlot requires a strictly ordered x
        const picked = [iMin, iMax, iNull].filter((i) => i >= 0).sort((a, c) => a - c);
        let previous = -1;
        for (const i of picked) {
            if (i === previous) {
                continue;
            }
            previous = i;
            outT.push(t[i] as number);
            outV.push(i === iNull ? null : (v[i] as number));
        }
    }
    return [outT, outV];
}
