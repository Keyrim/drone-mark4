/**
 * Pure time-axis math for the lane viewer: viewport transforms, tick
 * generation and time/value formatting. No DOM access (unit-testable).
 *
 * All times are in seconds relative to the origin of the current stream.
 */

/** Visible time range */
export interface Viewport {
    t0: number;
    t1: number;
}

export const MIN_SPAN_S = 0.001;

export function span(vp: Viewport): number {
    return vp.t1 - vp.t0;
}

/**
 * Zoom around an anchor time. factor > 1 zooms in, keeping anchorT at the
 * same on-screen position.
 */
export function zoom(vp: Viewport, anchorT: number, factor: number, maxSpan: number): Viewport {
    const oldSpan = span(vp);
    const newSpan = Math.min(Math.max(oldSpan / factor, MIN_SPAN_S), Math.max(maxSpan, MIN_SPAN_S));
    const ratio = oldSpan > 0 ? (anchorT - vp.t0) / oldSpan : 0.5;
    const t0 = anchorT - ratio * newSpan;
    return { t0, t1: t0 + newSpan };
}

export function pan(vp: Viewport, deltaT: number): Viewport {
    return { t0: vp.t0 + deltaT, t1: vp.t1 + deltaT };
}

/**
 * Keep at least a sliver of the recorded range [0, endT] visible, so the user
 * cannot scroll the data completely out of view.
 */
export function clampToData(vp: Viewport, endT: number): Viewport {
    const s = span(vp);
    const margin = s * 0.9;
    let t0 = vp.t0;
    if (t0 > endT - s * 0.1) {
        t0 = endT - s * 0.1;
    }
    if (t0 < -margin) {
        t0 = -margin;
    }
    return { t0, t1: t0 + s };
}

/** Smallest "nice" step (1, 2 or 5 times a power of ten) >= raw */
export function niceStep(raw: number): number {
    if (!(raw > 0) || !isFinite(raw)) {
        return 1;
    }
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    for (const m of [1, 2, 5]) {
        if (m * mag >= raw) {
            return m * mag;
        }
    }
    return 10 * mag;
}

/**
 * Nice tick positions covering [t0, t1], spaced ~targetPx apart on a widthPx
 * wide plot. Returns ascending times; the step is retrievable via tickStep().
 */
export function ticks(vp: Viewport, widthPx: number, targetPx = 90): number[] {
    const s = span(vp);
    if (!(s > 0) || !(widthPx > 0)) {
        return [];
    }
    const step = tickStep(vp, widthPx, targetPx);
    const out: number[] = [];
    const first = Math.ceil(vp.t0 / step - 1e-9) * step;
    for (let t = first; t <= vp.t1 + step * 1e-9 && out.length < 1000; t += step) {
        // Snap to the step grid so float drift does not accumulate
        out.push(Math.round(t / step) * step);
    }
    return out;
}

export function tickStep(vp: Viewport, widthPx: number, targetPx = 90): number {
    return niceStep((span(vp) * targetPx) / widthPx);
}

/**
 * Format a stream-relative time for the ruler, with a precision adapted to
 * the tick step (e.g. "12.35 s", "1:05", "-0.4 s").
 */
export function formatTick(t: number, step: number): string {
    const decimals = step >= 1 ? 0 : Math.min(6, Math.max(0, -Math.floor(Math.log10(step) + 1e-9)));
    const abs = Math.abs(t);
    if (abs >= 60 && step >= 1) {
        const sign = t < 0 ? "-" : "";
        const m = Math.floor(abs / 60);
        const s = Math.round(abs - m * 60);
        return `${sign}${m}:${String(s).padStart(2, "0")}`;
    }
    return `${t.toFixed(decimals)} s`;
}

/** Format a duration for the status bar (e.g. "1 min 23.4 s") */
export function formatDuration(seconds: number): string {
    const s = seconds < 0 ? 0 : seconds;
    if (s < 60) {
        return `${s.toFixed(1)} s`;
    }
    const m = Math.floor(s / 60);
    return `${m} min ${(s - m * 60).toFixed(1)} s`;
}

/** Compact value formatting for the lane readouts */
export function formatValue(v: number | null | undefined): string {
    if (v === null || v === undefined || !isFinite(v)) {
        return "-";
    }
    const abs = Math.abs(v);
    if (abs !== 0 && (abs >= 1e6 || abs < 1e-4)) {
        return v.toExponential(3);
    }
    // Up to 6 significant digits, trailing zeros trimmed
    return String(Number(v.toPrecision(6)));
}

/** Index of the last element of the ascending array t that is <= x, or -1 */
export function lowerBound(t: ArrayLike<number>, x: number): number {
    let lo = 0;
    let hi = t.length - 1;
    if (hi < 0 || t[0] > x) {
        return -1;
    }
    while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if (t[mid] <= x) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}
