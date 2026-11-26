import type { AutoscaleInfo, UTCTimestamp } from "lightweight-charts";

// Exact domain values and timestamps stay with providers. Ordinal chart times
// avoid rounding nanoseconds or inventing calendar minutes between observations.
export type PricePoint = {
  timestamp_ns: string;
  price: string | null;
  volume?: string;
  openInterest?: string;
};

export type Candle = {
  timestamp_ns: string;
  open: string;
  high: string;
  low: string;
  close: string;
  volume: string;
  openInterest?: string;
};

export type ChartSeries =
  | { kind: "line"; points: readonly PricePoint[] }
  | { kind: "candles"; points: readonly Candle[] }
  | { kind: "histogram"; points: readonly PricePoint[] };

export type ChartLine = { label: string; color: string; values: readonly (number | null)[] };

export type OscillatorSpec = {
  label: string;
  histogramLabel?: string;
  emptyLabel: string;
  lines: readonly ChartLine[];
  histogram: readonly (number | null)[];
};

// A trade at the candle with this index: buys below it, sells above it.
export type TradeMark = { index: number; side: "buy" | "sell"; text: string };

export type ChartPriceLine = {
  value: string;
  label: string;
  tone?: "neutral" | "up" | "down" | "accent";
  dashed?: boolean;
  axisLabelVisible?: boolean;
};

export type VerticalMarker = { index: number; label: string };

export const chartTime = (index: number) => (index + 1) as UTCTimestamp;

export const pointValue = (point: PricePoint | Candle) => {
  const raw = "close" in point ? point.close : point.price;
  return raw === "" ? null : raw;
};

// The axis steps in 1e-8, and its tick at zero can carry a floating-point residue.
export const numberText = (value: number) =>
  Math.abs(value) < 5e-9 ? "0" : Number(value.toPrecision(8)).toString();

export const withAlpha = (hex: string, alpha: string) =>
  /^#[0-9a-f]{6}$/i.test(hex) ? `${hex}${alpha}` : hex;

export const cssColor = (css: CSSStyleDeclaration, name: string, fallback: string) =>
  css.getPropertyValue(name).trim() || fallback;

export function defaultVisibleCount(
  kind: ChartSeries["kind"],
  count: number,
  containerWidth: number,
): number {
  if (kind !== "candles" || count <= 0) return count;
  const plotWidth = containerWidth > 0 ? Math.max(168, containerWidth - 56) : 420;
  return Math.min(count, Math.max(24, Math.floor(plotWidth / 7)));
}

// A quiet market spans at least 20 price ticks, so one tick is not the whole height.
export function minimumTickSpan(
  info: AutoscaleInfo | null,
  tick: number | null,
): AutoscaleInfo | null {
  if (!info?.priceRange || !tick) return info;
  const { minValue, maxValue } = info.priceRange;
  const missing = tick * 20 - (maxValue - minValue);
  if (missing <= 0) return info;
  return {
    ...info,
    priceRange: { minValue: minValue - missing / 2, maxValue: maxValue + missing / 2 },
  };
}

// Whitespace preserves an observation on the time axis, but Lightweight Charts
// connects lines over whitespace. Separate contiguous runs preserve real gaps.
export function chartRuns(values: readonly (number | null)[]) {
  const runs: { time: UTCTimestamp; value: number }[][] = [];
  let current: { time: UTCTimestamp; value: number }[] | undefined;
  values.forEach((value, index) => {
    if (value === null) {
      current = undefined;
      return;
    }
    if (!current) {
      current = [];
      runs.push(current);
    }
    current.push({ time: chartTime(index), value });
  });
  return runs;
}

export function visiblePriceExtremes(points: readonly Candle[], start: number, end: number) {
  const visible = points.slice(start, end);
  if (!visible.length) return [];
  const highAt = visible.reduce(
    (at, p, i) => (Number(p.high) > Number(visible[at].high) ? i : at),
    0,
  );
  const lowAt = visible.reduce((at, p, i) => (Number(p.low) < Number(visible[at].low) ? i : at), 0);
  return visible[highAt].high === visible[lowAt].low
    ? [{ index: start + highAt, price: visible[highAt].high }]
    : [
        { index: start + highAt, price: visible[highAt].high },
        { index: start + lowAt, price: visible[lowAt].low },
      ];
}
