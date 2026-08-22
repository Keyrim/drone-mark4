/**
 * Shared time ruler drawn on a canvas above the lanes. Shows nice time ticks
 * for the current viewport and a marker for the hovered cursor time.
 */

import { cssVar, type PlotBox } from "./lanes";
import { formatTick, span, tickStep, type Viewport } from "./timebase";

export const RULER_H = 24;

export class Ruler {
    private readonly ctx: CanvasRenderingContext2D;
    private lastW = 0;
    private lastDpr = 1;

    constructor(private readonly canvas: HTMLCanvasElement) {
        const ctx = canvas.getContext("2d");
        if (!ctx) {
            throw new Error("2D canvas context unavailable");
        }
        this.ctx = ctx;
    }

    render(vp: Viewport, ticks: number[], box: PlotBox, cursorT: number | null): void {
        const parent = this.canvas.parentElement;
        const w = parent ? parent.clientWidth : this.canvas.clientWidth;
        const dpr = window.devicePixelRatio || 1;
        if (w <= 0) {
            return;
        }
        if (w !== this.lastW || dpr !== this.lastDpr) {
            this.lastW = w;
            this.lastDpr = dpr;
            this.canvas.width = Math.round(w * dpr);
            this.canvas.height = Math.round(RULER_H * dpr);
            this.canvas.style.width = `${w}px`;
            this.canvas.style.height = `${RULER_H}px`;
        }

        const ctx = this.ctx;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, w, RULER_H);

        const s = span(vp);
        if (!(s > 0) || box.width <= 0) {
            return;
        }

        const textColor = cssVar("--fg-dim", "#8b95a6");
        const lineColor = "rgba(127, 127, 127, 0.45)";
        const accent = cssVar("--accent", "#3987e5");
        const font = `10px ${cssVar("--mono", "monospace")}`;

        const xOf = (t: number): number => box.left + ((t - vp.t0) / s) * box.width;
        const step = tickStep(vp, box.width);

        ctx.font = font;
        ctx.textBaseline = "middle";
        ctx.strokeStyle = lineColor;
        ctx.fillStyle = textColor;
        ctx.lineWidth = 1;

        for (const t of ticks) {
            const x = Math.round(xOf(t)) + 0.5;
            if (x < box.left - 1 || x > box.left + box.width + 1) {
                continue;
            }
            ctx.beginPath();
            ctx.moveTo(x, RULER_H - 7);
            ctx.lineTo(x, RULER_H - 1);
            ctx.stroke();
            ctx.textAlign = "center";
            ctx.fillText(formatTick(t, step), x, RULER_H / 2 - 3);
        }

        // Baseline separating the ruler from the lanes
        ctx.beginPath();
        ctx.moveTo(box.left, RULER_H - 0.5);
        ctx.lineTo(box.left + box.width, RULER_H - 0.5);
        ctx.stroke();

        // Hovered time marker
        if (cursorT !== null && cursorT >= vp.t0 && cursorT <= vp.t1) {
            const x = Math.round(xOf(cursorT)) + 0.5;
            ctx.strokeStyle = accent;
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, RULER_H);
            ctx.stroke();
        }
    }
}
