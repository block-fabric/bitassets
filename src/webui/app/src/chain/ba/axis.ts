// The numbers of a chart's axes and of what the pointer is over (ba/Chart.svelte). Prices in CHN can be
// tiny (0.000000012) or huge (12,000,000,000): an axis says them in a few characters, compactly (three
// significant digits, k/M/B/T, or 1.2e-8 below a thousandth), as many digits as its ticks need to differ;
// the readout under the pointer says the value exactly. User feedback 2026-10-19: long labels spilled off
// the left of the chart.

const SUFFIXES: readonly [number, string][] = [
  [1e12, 'T'],
  [1e9, 'B'],
  [1e6, 'M'],
  [1e3, 'k'],
];

/** "1.50" → "1.5", "2.00" → "2", "1.20e-8" → "1.2e-8". */
function trimZeros(s: string): string {
  const [m, e] = s.split('e');
  const t = m.includes('.') ? m.replace(/0+$/, '').replace(/\.$/, '') : m;
  return e === undefined ? t : `${t}e${e.replace('+', '')}`;
}

/** A value of an axis in a few characters: `sig` significant digits, k/M/B/T, scientific below 0.001. */
export function compactNumber(v: number, sig = 3): string {
  if (!Number.isFinite(v)) return '–';
  if (v === 0) return '0';
  const sign = v < 0 ? '−' : '';
  const a = Number(Math.abs(v).toPrecision(sig));
  if (a >= 1e15) return sign + trimZeros(a.toExponential(sig - 1));
  for (const [n, s] of SUFFIXES) if (a >= n) return sign + trimZeros((a / n).toPrecision(sig)) + s;
  if (a >= 1e-3) return sign + trimZeros(a.toPrecision(sig));
  return sign + trimZeros(a.toExponential(sig - 1));
}

/** The labels of the ticks of an axis: compact, with the fewest significant digits (3 or more) that tell them apart. */
export function axisLabels(values: readonly number[]): string[] {
  let labels: string[] = [];
  for (let sig = 3; sig <= 9; sig++) {
    labels = values.map((v) => compactNumber(v, sig));
    if (new Set(labels).size === new Set(values).size) return labels;
  }
  return labels;
}

/** A value exactly as far as a number holds it (12 significant digits), in plain decimals with thousands grouped. */
export function exactNumber(v: number): string {
  if (!Number.isFinite(v)) return '–';
  if (v === 0) return '0';
  const sign = v < 0 ? '−' : '';
  const a = Math.abs(v);
  const exp = Math.floor(Math.log10(a));
  const decimals = Math.min(100, Math.max(0, 11 - exp));
  const fixed = Number(a.toPrecision(12)).toFixed(decimals);
  const [w, f = ''] = fixed.split('.');
  const frac = f.replace(/0+$/, '');
  return sign + w.replace(/\B(?=(\d{3})+(?!\d))/g, ',') + (frac ? `.${frac}` : '');
}

/** The width in pixels a label of the axis takes, at its 10.5 px tabular figures (an estimate that errs wide). */
export function labelWidth(text: string, px = 10.5): number {
  return Math.ceil(text.length * px * 0.62);
}
