/**
 * Lane rendering: one uPlot chart per lane, stacked vertically, sharing a
 * synchronized cursor and an externally driven x viewport. A lane draws one
 * or more series; its label chip lists each one with its color, name and
 * value readout.
 */

import uPlot from "uplot";

import { groupByLane, type LaneConfig, type SeriesBuffer } from "./model";
import { formatValue, lowerBound, type Viewport } from "./timebase";

const LANE_H_DEFAULT = 110;
const LANE_H_MIN = 48;
const LANE_H_MAX = 500;
const Y_AXIS_W = 56;
const SYNC_KEY = "mark4-lanes";

export interface PlotBox {
    /** Left edge of the plotting area in CSS px, relative to the container */
    left: number;
    /** Width of the plotting area in CSS px */
    width: number;
}

export interface LanesCallbacks {
    /** Hovered time changed (null when the cursor left the lanes) */
    onCursorTime(t: number | null): void;
    /** Layout changed, a re-render is needed */
    onNeedsRender(): void;
}

export function cssVar(name: string, fallback: string): string {
    const value = getComputedStyle(document.body).getPropertyValue(name).trim();
    return value !== "" ? value : fallback;
}

class Lane {
    readonly root: HTMLElement;
    private readonly plotEl: HTMLElement;
    private readonly valueEls: HTMLElement[] = [];
    private readonly lastValueTexts: string[] = [];
    private plot: uPlot | null = null;
    private lastTotalLen = -1;
    private hoverT: number | null = null;
    private height: number;

    constructor(
        private readonly buffers: SeriesBuffer[],
        config: LaneConfig,
        private readonly getViewport: () => Viewport,
        private readonly getTicks: () => number[],
        private readonly decimate: (t: number[], v: (number | null)[]) => uPlot.AlignedData,
        private readonly cb: LanesCallbacks
    ) {
        this.height = Math.min(LANE_H_MAX, LANE_H_DEFAULT + (buffers.length - 1) * 16);

        this.root = document.createElement("div");
        this.root.className = "lane";

        const label = document.createElement("div");
        label.className = "lane-label";

        const title = document.createElement("div");
        title.className = "lane-title";
        title.textContent = config.title;
        label.appendChild(title);

        const rows = document.createElement("div");
        rows.className = "lane-rows";
        for (const buffer of buffers) {
            const row = document.createElement("div");
            row.className = "lane-row";
            const dot = document.createElement("span");
            dot.className = "lane-dot";
            dot.style.background = buffer.def.color;
            if (buffer.def.dash) {
                dot.classList.add("dashed");
            }
            const name = document.createElement("span");
            name.className = "lane-row-name";
            name.textContent = buffer.def.label;
            const value = document.createElement("span");
            value.className = "lane-row-value";
            value.textContent = "-";
            row.appendChild(dot);
            row.appendChild(name);
            row.appendChild(value);
            rows.appendChild(row);
            this.valueEls.push(value);
            this.lastValueTexts.push("-");
        }
        label.appendChild(rows);

        const footer = document.createElement("div");
        footer.className = "lane-footer";
        footer.textContent = buffers[0]?.def.unit ?? "";
        label.appendChild(footer);

        this.plotEl = document.createElement("div");
        this.plotEl.className = "lane-plot";

        const resize = document.createElement("div");
        resize.className = "lane-resize";
        this.setupResize(resize);

        this.root.appendChild(label);
        this.root.appendChild(this.plotEl);
        this.root.appendChild(resize);
    }

    /**
     * Chart data for uPlot. Multi-series lanes are aligned onto a union time
     * axis: alignment holes become undefined (the line stays connected),
     * explicit null gap markers survive as real line breaks.
     */
    private joinedData(): uPlot.AlignedData {
        const first = this.buffers[0];
        if (first === undefined) {
            return [[]] as unknown as uPlot.AlignedData;
        }
        if (this.buffers.length === 1) {
            return this.decimate(first.t, first.v);
        }
        return uPlot.join(this.buffers.map((b) => this.decimate(b.t, b.v)));
    }

    private totalLen(): number {
        return this.buffers.reduce((n, b) => n + b.t.length, 0);
    }

    build(width: number): void {
        const gridColor = "rgba(127, 127, 127, 0.18)";
        const axisText = cssVar("--fg-dim", "#8b95a6");
        const font = `10px ${cssVar("--mono", "monospace")}`;

        const series: uPlot.Series[] = [{}];
        for (const buffer of this.buffers) {
            const one: uPlot.Series = {
                stroke: buffer.def.color,
                width: buffer.def.dash ? 1 : 2,
                points: { show: false },
                spanGaps: false,
            };
            if (buffer.def.dash) {
                one.dash = buffer.def.dash;
            }
            series.push(one);
        }

        const opts: uPlot.Options = {
            width: Math.max(50, width),
            height: this.height,
            padding: [6, 6, 0, 0],
            legend: { show: false },
            cursor: {
                sync: { key: SYNC_KEY, setSeries: false },
                drag: { setScale: false, x: false, y: false },
                y: false,
                points: { size: 6 },
            },
            scales: {
                x: {
                    time: false,
                    auto: false,
                    range: (): [number, number] => {
                        const vp = this.getViewport();
                        return [vp.t0, vp.t1];
                    },
                },
                y: { auto: true },
            },
            axes: [
                {
                    // x axis: grid aligned with the shared ruler, no labels
                    size: 4,
                    stroke: axisText,
                    ticks: { show: false },
                    grid: { show: true, stroke: gridColor, width: 1 },
                    splits: (_u: uPlot, _idx: number, min: number, max: number): number[] =>
                        this.getTicks().filter((t) => t >= min && t <= max),
                    values: (_u: uPlot, splits: number[]): string[] => splits.map(() => ""),
                },
                {
                    size: Y_AXIS_W,
                    gap: 4,
                    stroke: axisText,
                    font,
                    ticks: { show: false },
                    grid: { show: true, stroke: gridColor, width: 1 },
                },
            ],
            series,
            hooks: {
                setCursor: [
                    (u: uPlot): void => {
                        const left = u.cursor.left ?? -1;
                        this.hoverT = left >= 0 ? u.posToVal(left, "x") : null;
                        this.cb.onCursorTime(this.hoverT);
                        this.updateValues();
                    },
                ],
            },
        };

        this.plot = new uPlot(opts, this.joinedData(), this.plotEl);
        this.lastTotalLen = this.totalLen();
    }

    private setupResize(handle: HTMLElement): void {
        let startY = 0;
        let startH = 0;
        handle.addEventListener("pointerdown", (e: PointerEvent) => {
            e.preventDefault();
            startY = e.clientY;
            startH = this.height;
            handle.setPointerCapture(e.pointerId);
        });
        handle.addEventListener("pointermove", (e: PointerEvent) => {
            if (!handle.hasPointerCapture(e.pointerId)) {
                return;
            }
            this.height = Math.min(LANE_H_MAX, Math.max(LANE_H_MIN, startH + (e.clientY - startY)));
            if (this.plot) {
                this.plot.setSize({ width: this.plot.width, height: this.height });
            }
            this.cb.onNeedsRender();
        });
    }

    setWidth(width: number): void {
        if (this.plot && width > 0) {
            this.plot.setSize({ width, height: this.height });
        }
    }

    /** Apply the viewport and any new data. Called once per rendered frame. */
    render(vp: Viewport, force: boolean): void {
        if (!this.plot) {
            return;
        }
        const total = this.totalLen();
        if (force || total !== this.lastTotalLen) {
            this.lastTotalLen = total;
            this.plot.setData(this.joinedData(), false);
        }
        this.plot.setScale("x", { min: vp.t0, max: vp.t1 });
        this.updateValues();
    }

    private updateValues(): void {
        for (let i = 0; i < this.buffers.length; i++) {
            const b = this.buffers[i] as SeriesBuffer;
            let text: string;
            if (this.hoverT !== null) {
                const idx = lowerBound(b.t, this.hoverT);
                text = idx >= 0 ? formatValue(b.v[idx]) : "-";
            } else {
                text = formatValue(b.last());
            }
            if (text !== this.lastValueTexts[i]) {
                this.lastValueTexts[i] = text;
                (this.valueEls[i] as HTMLElement).textContent = text;
            }
        }
    }

    /** Plot area box relative to the lanes container, in CSS px */
    box(): PlotBox | null {
        if (!this.plot) {
            return null;
        }
        const dpr = window.devicePixelRatio || 1;
        return {
            left: this.plotEl.offsetLeft + this.plot.bbox.left / dpr,
            width: this.plot.bbox.width / dpr,
        };
    }

    destroy(): void {
        if (this.plot) {
            this.plot.destroy();
            this.plot = null;
        }
        this.root.remove();
    }
}

export class LanesView {
    private lanes: Lane[] = [];
    private currentTicks: number[] = [];
    private viewport: Viewport = { t0: 0, t1: 20 };
    private readonly observer: ResizeObserver;
    /** Rendering hook the pages replace with a decimating one */
    decimate: (t: number[], v: (number | null)[]) => uPlot.AlignedData = (t, v) =>
        [t, v] as uPlot.AlignedData;

    constructor(
        private readonly container: HTMLElement,
        private readonly cb: LanesCallbacks
    ) {
        this.observer = new ResizeObserver(() => this.resizeLanes());
        this.observer.observe(container);
    }

    private resizeLanes(): void {
        const width = this.plotWidth();
        for (const lane of this.lanes) {
            lane.setWidth(width);
        }
        this.cb.onNeedsRender();
    }

    private plotWidth(): number {
        return Math.max(50, this.container.clientWidth - this.labelWidth());
    }

    private labelWidth(): number {
        const raw = getComputedStyle(this.container).getPropertyValue("--lane-label-w").trim();
        const parsed = Number.parseFloat(raw);
        return Number.isFinite(parsed) ? parsed : 200;
    }

    /** Rebuild all lanes from the given config and buffers. */
    setLanes(lanes: LaneConfig[], buffers: Map<string, SeriesBuffer>): void {
        this.clear();
        const width = this.plotWidth();
        for (const group of groupByLane(lanes, buffers)) {
            const lane = new Lane(
                group.buffers,
                group.config,
                () => this.viewport,
                () => this.currentTicks,
                (t, v) => this.decimate(t, v),
                this.cb
            );
            this.container.appendChild(lane.root);
            lane.build(width);
            this.lanes.push(lane);
        }
    }

    clear(): void {
        for (const lane of this.lanes) {
            lane.destroy();
        }
        this.lanes = [];
    }

    isEmpty(): boolean {
        return this.lanes.length === 0;
    }

    render(vp: Viewport, ticks: number[], force = false): void {
        this.viewport = vp;
        this.currentTicks = ticks;
        for (const lane of this.lanes) {
            lane.render(vp, force);
        }
    }

    /** Plot area of the first lane (all lanes share the same geometry) */
    box(): PlotBox | null {
        return this.lanes[0]?.box() ?? null;
    }
}
