// The axes of the charts of prices (user feedback 2026-10-19): CHN prices with many decimals spilled off the
// left of the chart. The axis says them compactly and makes room for them; the readout says them exactly.
import { render } from '@testing-library/svelte';
import { beforeAll, describe, expect, it } from 'vitest';
import Chart from './Chart.svelte';
import { axisLabels, compactNumber, exactNumber, labelWidth } from './axis';

describe('the numbers of an axis', () => {
  it('are compact: three significant digits, k/M/B/T, scientific for tiny values', () => {
    expect(compactNumber(0)).toBe('0');
    expect(compactNumber(1)).toBe('1');
    expect(compactNumber(0.5)).toBe('0.5');
    expect(compactNumber(123.456)).toBe('123');
    expect(compactNumber(0.00123456)).toBe('0.00123');
    expect(compactNumber(0.000000012345)).toBe('1.23e-8');
    expect(compactNumber(1e-12)).toBe('1e-12');
    expect(compactNumber(1234)).toBe('1.23k');
    expect(compactNumber(999_999)).toBe('1M');
    expect(compactNumber(12_345_678_901)).toBe('12.3B');
    expect(compactNumber(4.2e15)).toBe('4.2e15');
    expect(compactNumber(-2500)).toBe('−2.5k');
  });

  it('take as many digits as the ticks need to differ', () => {
    expect(axisLabels([1.0001e-8, 1.0002e-8, 1.0003e-8])).toEqual(['1.0001e-8', '1.0002e-8', '1.0003e-8']);
    expect(axisLabels([100, 150, 200])).toEqual(['100', '150', '200']);
    expect(axisLabels([1_000_000, 1_000_500, 1_001_000])).toEqual(['1M', '1.0005M', '1.001M']);
  });

  it('say the value exactly in the readout, in plain decimals', () => {
    expect(exactNumber(0.000000012345)).toBe('0.000000012345');
    expect(exactNumber(12_345_678_901)).toBe('12,345,678,901');
    expect(exactNumber(1.5)).toBe('1.5');
    expect(exactNumber(0)).toBe('0');
  });
});

describe('a chart of prices', () => {
  beforeAll(() => {
    // bind:clientWidth observes the size (jsdom has no ResizeObserver): the chart keeps its 520 px.
    (globalThis as Record<string, unknown>).ResizeObserver ??= class {
      observe() {}
      unobserve() {}
      disconnect() {}
    };
  });

  /** Each label of the y axis fits between the left edge and the plot. */
  function fitsLeft(container: HTMLElement): void {
    const labels = [...container.querySelectorAll<SVGTextElement>('text.axis[text-anchor="end"]')].filter((e) => !!e.querySelector('title'));
    expect(labels.length).toBe(3);
    for (const l of labels) {
      const text = [...l.childNodes].filter((n) => n.nodeType === 3).map((n) => n.textContent).join('');
      expect(text.length, text).toBeLessThanOrEqual(10);
      expect(labelWidth(text), `${text} spills off the left`).toBeLessThanOrEqual(Number(l.getAttribute('x')));
      // The exact value in its tooltip.
      expect(l.querySelector('title')?.textContent).toMatch(/^[\d,.]+$/);
    }
  }

  it('keeps tiny prices on the chart, compactly', () => {
    const points = [0.000000012345, 0.000000012399, 0.000000012601].map((y, i) => ({ x: 100 + i, y }));
    const { container } = render(Chart, { props: { points, label: 'price' } });
    fitsLeft(container);
    expect(container.textContent).toMatch(/e-8/);
  });

  it('keeps huge prices on the chart, compactly', () => {
    const points = [12_345_678_901, 12_945_678_901, 13_345_678_901].map((y, i) => ({ x: 100 + i, y }));
    const { container } = render(Chart, { props: { points, label: 'price' } });
    fitsLeft(container);
    expect(container.textContent).toMatch(/B/);
  });
});
