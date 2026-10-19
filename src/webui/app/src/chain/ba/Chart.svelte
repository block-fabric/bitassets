<script lang="ts">
  // A line over a range of x (blocks, or what a pool holds), drawn in SVG: the line, the area under it, the
  // points marked, a dashed line of comparison, a vertical mark ("now"), and what the pointer is over. The
  // charts of BitAssets: a pool's price as the app saw it, an auction's price falling block by block, the
  // curve of a pool and where a trade moves it.
  //
  // The axis of y says its ticks compactly (axis.ts: 1.2e-8, 3.4M), and makes room on the left for the
  // longest of them; the readout under the pointer says the value exactly (exactNumber).
  import { axisLabels, exactNumber, labelWidth } from './axis';

  interface Pt {
    x: number;
    y: number;
  }
  let {
    points,
    compare = [],
    marks = [],
    nowX = null,
    tall = 200,
    step = false,
    area = true,
    label,
    formatX = (x: number) => String(Math.round(x)),
    formatY = exactNumber,
    formatTick = undefined,
    yZero = false,
    logX = false,
  }: {
    points: readonly Pt[];
    /** A second line, dashed (the price before a trade, the end price). */
    compare?: readonly Pt[];
    /** Points marked with a dot (alt: in the colour of a warning). */
    marks?: readonly (Pt & { alt?: boolean })[];
    nowX?: number | null;
    tall?: number;
    /** A step line: a value holds until the next point. */
    step?: boolean;
    area?: boolean;
    /** What the chart shows, for a screen reader. */
    label: string;
    formatX?: (x: number) => string;
    /** A value of y in the readout (exact by default). */
    formatY?: (y: number) => string;
    /** The ticks of the y axis (compact, as many digits as they need to differ, by default). */
    formatTick?: (ys: readonly number[]) => string[];
    /** The y axis from zero. */
    yZero?: boolean;
    logX?: boolean;
  } = $props();

  let width = $state(520);
  const all = $derived([...points, ...compare, ...marks]);
  const fx = (x: number) => (logX ? Math.log(Math.max(x, 1e-12)) : x);
  const xs = $derived(all.map((p) => fx(p.x)));
  const ys = $derived(all.map((p) => p.y));
  const x0 = $derived(xs.length ? Math.min(...xs) : 0);
  const x1 = $derived(xs.length ? Math.max(...xs, x0 + 1e-9) : 1);
  const yMax = $derived(ys.length ? Math.max(...ys) : 1);
  const yMinRaw = $derived(ys.length ? Math.min(...ys) : 0);
  /** The span of y, never nothing (a single price): a tenth of the value around it. */
  const spanY = $derived(yMax - (yZero ? 0 : yMinRaw) || Math.abs(yMax) * 0.2 || 1);
  const y0 = $derived(yZero ? 0 : yMinRaw - spanY * 0.08 - (yMax === yMinRaw ? spanY / 2 : 0));
  const y1 = $derived(yMax + spanY * 0.08 + (yMax === yMinRaw && !yZero ? spanY / 2 : 0));
  const ticks = $derived([0, 0.5, 1].map((f) => y0 + (y1 - y0) * f));
  const tickLabels = $derived(formatTick ? formatTick(ticks) : axisLabels(ticks));
  // The left margin is as wide as the longest label of the axis and its gap (the labels are short: axis.ts).
  const pad = $derived({ l: Math.max(28, Math.max(...tickLabels.map((l) => labelWidth(l))) + 10), r: 12, t: 12, b: 24 });
  const X = (x: number) => pad.l + ((fx(x) - x0) / (x1 - x0 || 1)) * Math.max(1, width - pad.l - pad.r);
  const Y = (y: number) => pad.t + (1 - (y - y0) / (y1 - y0 || 1)) * (tall - pad.t - pad.b);

  function pathOf(list: readonly Pt[]): string {
    return list
      .map((p, i) => {
        const x = X(p.x).toFixed(1);
        const y = Y(p.y).toFixed(1);
        if (i === 0) return `M${x} ${y}`;
        return step ? `H${x}V${y}` : `L${x} ${y}`;
      })
      .join('');
  }
  const line = $derived(pathOf(points));
  const areaPath = $derived(points.length > 1 && area ? `${line}V${Y(Math.max(y0, 0)).toFixed(1)}H${X(points[0].x).toFixed(1)}Z` : '');
  const comparePath = $derived(pathOf(compare));

  let hover = $state<Pt | null>(null);
  function move(e: PointerEvent) {
    if (points.length === 0) return;
    const r = (e.currentTarget as SVGElement).getBoundingClientRect();
    const px = e.clientX - r.left;
    let best = points[0];
    for (const p of points) if (Math.abs(X(p.x) - px) < Math.abs(X(best.x) - px)) best = p;
    hover = best;
  }
</script>

<div class="ba-chart" bind:clientWidth={width}>
  {#if points.length === 0}
    <p class="ba-empty">Nothing to draw yet.</p>
  {:else}
    <svg {width} height={tall} viewBox="0 0 {width} {tall}" role="img" aria-label={label} onpointermove={move} onpointerleave={() => (hover = null)}>
      {#each ticks as tk, i (i)}
        <line class="grid" x1={pad.l} x2={width - pad.r} y1={Y(tk)} y2={Y(tk)} />
        <text class="axis" x={pad.l - 6} y={Y(tk) + 3.5} text-anchor="end"><title>{formatY(tk)}</title>{tickLabels[i]}</text>
      {/each}
      <text class="axis" x={pad.l} y={tall - 6}>{formatX(logX ? Math.exp(x0) : x0)}</text>
      <text class="axis" x={width - pad.r} y={tall - 6} text-anchor="end">{formatX(logX ? Math.exp(x1) : x1)}</text>
      {#if areaPath}<path class="area" d={areaPath} />{/if}
      {#if comparePath}<path class="line alt" d={comparePath} />{/if}
      <path class="line" d={line} />
      {#if nowX !== null}<line class="now" x1={X(nowX)} x2={X(nowX)} y1={pad.t} y2={tall - pad.b} />{/if}
      {#each marks as m, i (i)}<circle class="dot" class:alt={m.alt} cx={X(m.x)} cy={Y(m.y)} r="4.5" />{/each}
      {#if hover}<circle class="dot" cx={X(hover.x)} cy={Y(hover.y)} r="3.5" />{/if}
    </svg>
    {#if hover}<span class="readout">{formatX(hover.x)} · {formatY(hover.y)}</span>{/if}
  {/if}
</div>
