import { useCallback, useEffect, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import {
  AreaSeries,
  BaselineSeries,
  CandlestickSeries,
  ColorType,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  LineStyle,
  createChart,
  createSeriesMarkers,
  type AutoscaleInfo,
  type IChartApi,
  type IPanePrimitive,
  type IPanePrimitivePaneView,
  type IPriceLine,
  type IPrimitivePaneRenderer,
  type ISeriesApi,
  type ISeriesMarkersPluginApi,
  type SeriesType,
  type Time,
  type UTCTimestamp,
  type LogicalRange,
} from "lightweight-charts";
import { Icon } from "../ui/Icon";
import { usePreferences } from "../ui/preferences";
import "./price-chart.css";

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
type Series =
  | { kind: "line"; points: readonly PricePoint[] }
  | { kind: "candles"; points: readonly Candle[] }
  | { kind: "histogram"; points: readonly PricePoint[] };
type Line = { label: string; color: string; values: readonly (number | null)[] };
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
type Props = {
  series: Series;
  label: string;
  // Shows the label before the readout, where nothing else names the chart.
  titled?: boolean;
  volumeLabel: string;
  openInterestLabel?: string;
  formatTime: (value: string) => string;
  formatAxisTime?: (value: string) => string;
  describe: (index: number) => ReactNode;
  controls?: { zoomIn: string; zoomOut: string; earlier: string; later: string; latest: string };
  showVolume?: boolean;
  compact?: boolean;
  fillArea?: boolean;
  baselineValue?: number;
  fitHeight?: boolean;
  overlays?: readonly Line[];
  oscillator?: {
    label: string;
    histogramLabel?: string;
    emptyLabel: string;
    lines: readonly Line[];
    histogram: readonly (number | null)[];
  };
  referencePrice?: { value: string; label: string };
  priceLines?: readonly ChartPriceLine[];
  verticalMarkers?: readonly VerticalMarker[];
  focusedTime?: string | null;
  onFocusChange?: (time: string | null) => void;
  showPriceExtremes?: boolean;
  trades?: readonly TradeMark[];
  formatIndicatorValue?: (value: number) => string;
  formatPrice?: (value: number) => string;
  // The contract's minimum price move, as decimal text; the axis steps by it.
  priceTick?: string;
};
const emptyLines: readonly Line[] = [];
const emptyPriceLines: readonly ChartPriceLine[] = [];
const emptyVerticalMarkers: readonly VerticalMarker[] = [];
const noTrades: readonly TradeMark[] = [];
const time = (index: number) => (index + 1) as UTCTimestamp;
const valueAt = (point: PricePoint | Candle) => {
  const raw = "close" in point ? point.close : point.price;
  return raw === "" ? null : raw;
};
// The axis steps in 1e-8, and its tick at zero can carry a floating-point residue.
export const numberText = (value: number) =>
  Math.abs(value) < 5e-9 ? "0" : Number(value.toPrecision(8)).toString();
const withAlpha = (hex: string, alpha: string) =>
  /^#[0-9a-f]{6}$/i.test(hex) ? `${hex}${alpha}` : hex;

export function defaultVisibleCount(
  kind: Series["kind"],
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
    current.push({ time: time(index), value });
  });
  return runs;
}

function verticalLinePrimitive(
  api: IChartApi,
  items: readonly VerticalMarker[],
  stroke: string,
  fill: string,
  text: string,
): IPanePrimitive<Time> {
  const renderer: IPrimitivePaneRenderer = {
    draw(target) {
      target.useMediaCoordinateSpace(({ context, mediaSize }) => {
        context.save();
        context.font = "500 11px ui-sans-serif, system-ui, sans-serif";
        context.textBaseline = "middle";
        for (const item of items) {
          const rawX = api.timeScale().timeToCoordinate(time(item.index));
          if (rawX === null) continue;
          const x = Math.round(rawX) + 0.5;
          context.strokeStyle = stroke;
          context.lineWidth = 1;
          context.setLineDash([4, 3]);
          context.beginPath();
          context.moveTo(x, 0);
          context.lineTo(x, mediaSize.height);
          context.stroke();
          context.setLineDash([]);
          if (!item.label) continue;
          const padX = 6;
          const width = Math.ceil(context.measureText(item.label).width) + padX * 2;
          const height = 18;
          const left = x + 4 + width > mediaSize.width ? Math.max(2, x - width - 4) : x + 4;
          context.fillStyle = fill;
          context.strokeStyle = stroke;
          context.beginPath();
          context.roundRect(left, 6, width, height, 2);
          context.fill();
          context.stroke();
          context.fillStyle = text;
          context.fillText(item.label, left + padX, 6 + height / 2);
        }
        context.restore();
      });
    },
  };
  const views: readonly IPanePrimitivePaneView[] = [
    { zOrder: () => "top", renderer: () => renderer },
  ];
  return { paneViews: () => views };
}

function priceExtremesPrimitive(
  api: IChartApi,
  candles: ISeriesApi<SeriesType>,
  items: readonly { index: number; price: string }[],
  color: string,
  fontFamily: string,
  fontSize: number,
): IPanePrimitive<Time> {
  const renderer: IPrimitivePaneRenderer = {
    draw(target) {
      target.useMediaCoordinateSpace(({ context, mediaSize }) => {
        context.save();
        context.font = `500 ${fontSize}px ${fontFamily}`;
        context.textBaseline = "middle";
        context.fillStyle = color;
        for (const item of items) {
          const x = api.timeScale().timeToCoordinate(time(item.index));
          const y = candles.priceToCoordinate(Number(item.price));
          if (x === null || y === null) continue;
          const rightSide = x >= mediaSize.width / 2;
          const label = rightSide ? `${item.price} →` : `← ${item.price}`;
          const width = context.measureText(label).width;
          const clampedY = Math.max(8, Math.min(mediaSize.height - 8, y));
          const left = rightSide
            ? Math.max(4, x - width - 3)
            : Math.min(mediaSize.width - width - 4, x + 3);
          context.fillText(label, left, clampedY);
        }
        context.restore();
      });
    },
  };
  const views: readonly IPanePrimitivePaneView[] = [
    { zOrder: () => "top", renderer: () => renderer },
  ];
  return { paneViews: () => views };
}

export function PriceChart(props: Props) {
  const {
    series,
    label,
    titled = false,
    volumeLabel,
    openInterestLabel,
    describe,
    controls,
    showVolume = true,
    compact = false,
    fillArea = false,
    baselineValue,
    fitHeight = false,
    overlays = emptyLines,
    oscillator,
    referencePrice,
    priceLines = emptyPriceLines,
    verticalMarkers = emptyVerticalMarkers,
    focusedTime,
    onFocusChange,
    showPriceExtremes = false,
    trades = noTrades,
    formatIndicatorValue = numberText,
    priceTick,
  } = props;
  const container = useRef<HTMLDivElement>(null);
  const chart = useRef<IChartApi | null>(null);
  const markers = useRef<ISeriesMarkersPluginApi<Time> | null>(null);
  const drawnLines = useRef<{ series: ISeriesApi<SeriesType>; line: IPriceLine }[]>([]);
  const userLines = useRef<{ series: ISeriesApi<SeriesType>; line: IPriceLine }[]>([]);
  const primarySeriesRef = useRef<ISeriesApi<SeriesType> | null>(null);
  const verticalPrimitive = useRef<IPanePrimitive<Time> | null>(null);
  const extremesPrimitive = useRef<IPanePrimitive<Time> | null>(null);
  const layers = useRef(new Map<string, ISeriesApi<SeriesType>>());
  const latest = useRef(props);
  const saved = useRef<{
    start: number;
    end: number;
    anchor: string | null;
    size: number;
    custom: boolean;
  }>({
    start: 0,
    end: 0,
    anchor: null,
    size: 0,
    custom: false,
  });
  const updating = useRef(false);
  const { colors: quoteColors } = usePreferences();
  const [domQuoteColors, setDomQuoteColors] = useState(
    () => typeof document !== "undefined" && document.documentElement.dataset.quoteColors,
  );
  useEffect(() => {
    if (typeof MutationObserver === "undefined") return;
    const observer = new MutationObserver(() =>
      setDomQuoteColors(document.documentElement.dataset.quoteColors),
    );
    observer.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ["data-quote-colors"],
    });
    return () => observer.disconnect();
  }, []);
  const [focus, setFocus] = useState<string | null>(null);
  const activeFocus = focusedTime !== undefined ? focusedTime : focus;
  const updateFocus = (next: string | null) => {
    setFocus(next);
    onFocusChange?.(next);
  };
  // Where the volume pane starts inside the chart, for its label.
  const [volumeAt, setVolumeAt] = useState<{ top: number; left: number } | null>(null);
  const hasReference = referencePrice !== undefined;
  const placeVolumeLabel = useCallback(() => {
    const element = container.current;
    const api = chart.current;
    if (!element || !api) return;
    const paneHeight = api.panes()[0]?.getHeight();
    if (paneHeight === undefined) return;
    const top = element.offsetTop + paneHeight + 1;
    const left = api.options().leftPriceScale.visible ? api.priceScale("left").width() : 0;
    setVolumeAt(prev => (prev && prev.top === top && prev.left === left ? prev : { top, left }));
  }, []);
  useEffect(() => {
    if (!showVolume) return;
    const element = container.current!;
    // The engine resizes itself on the next frame.
    const observer = new ResizeObserver(() => requestAnimationFrame(placeVolumeLabel));
    observer.observe(element);
    return () => observer.disconnect();
  }, [showVolume, placeVolumeLabel]);
  useEffect(() => {
    if (!showVolume) return;
    const frame = requestAnimationFrame(placeVolumeLabel);
    return () => cancelAnimationFrame(frame);
  }, [showVolume, oscillator, hasReference, placeVolumeLabel]);
  const [view, setView] = useState({ start: 0, end: series.points.length });
  const points = series.points,
    count = points.length;
  const start = Math.min(view.start, Math.max(0, count - 1));
  const end = Math.max(start + 1, Math.min(view.end, count));
  const size = end - start;
  const selected =
    activeFocus === null ? -1 : points.findIndex(p => p.timestamp_ns === activeFocus);
  let lastObserved = end - 1;
  while (count && lastObserved > start && valueAt(points[lastObserved]) === null) lastObserved--;
  const index = Math.max(start, Math.min(end - 1, selected < 0 ? lastObserved : selected));

  const recordRange = useCallback((first: number, last: number, custom: boolean) => {
    const p = latest.current.series.points;
    saved.current = {
      start: first,
      end: last,
      anchor: last === p.length || !last ? null : p[last - 1].timestamp_ns,
      size: last - first,
      custom,
    };
    setView(prev =>
      prev.start === first && prev.end === last ? prev : { start: first, end: last },
    );
  }, []);
  useLayoutEffect(() => {
    latest.current = props;
  });
  useLayoutEffect(() => {
    const element = container.current;
    if (!element) return;
    const css = getComputedStyle(element);
    const text = css.getPropertyValue("--muted").trim() || "#999faa";
    const grid = css.getPropertyValue("--grid").trim() || "#23262c";
    const line = css.getPropertyValue("--line").trim() || "#343842";
    const when = (at: Time, axis: boolean) => {
      const p = latest.current;
      const point = p.series.points[Number(at) - 1];
      return point
        ? (axis ? (p.formatAxisTime ?? p.formatTime) : p.formatTime)(point.timestamp_ns)
        : "";
    };
    const api = createChart(element, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: "transparent" },
        textColor: text,
        fontSize: Number.parseFloat(css.getPropertyValue("--font-size-xs")) || 11,
        attributionLogo: false,
        panes: { separatorColor: line, separatorHoverColor: line },
      },
      grid: { vertLines: { color: grid }, horzLines: { color: grid } },
      crosshair: { mode: CrosshairMode.Normal },
      timeScale: {
        tickMarkFormatter: (t: Time) => when(t, true),
        tickMarkMaxCharacterLength: 16,
        rightOffset: 0,
        lockVisibleTimeRangeOnResize: true,
        fixLeftEdge: true,
        fixRightEdge: true,
        minBarSpacing: 0.0001,
      },
      localization: { timeFormatter: (t: Time) => when(t, false) },
    });
    chart.current = api;
    const rangeChanged = (range: LogicalRange | null) => {
      if (!range || updating.current) return;
      const p = latest.current.series.points;
      const first = Math.max(0, Math.round(range.from));
      const last = Math.min(p.length, Math.round(range.to) + 1);
      if (!p.length || last <= first) return;
      if (first === saved.current.start && last === saved.current.end) return;
      recordRange(first, last, true);
    };
    api.timeScale().subscribeVisibleLogicalRangeChange(rangeChanged);
    const resizeObserver =
      typeof ResizeObserver === "undefined"
        ? null
        : new ResizeObserver(() => {
            if (saved.current.custom) return;
            const currentSeries = latest.current.series;
            const total = currentSeries.points.length;
            if (!total || currentSeries.kind !== "candles") return;
            const width = defaultVisibleCount(currentSeries.kind, total, element.clientWidth);
            const first = Math.max(0, total - width);
            if (first === saved.current.start && total === saved.current.end) return;
            updating.current = true;
            recordRange(first, total, false);
            api.timeScale().setVisibleLogicalRange({ from: first, to: total - 1 });
            updating.current = false;
          });
    resizeObserver?.observe(element);
    api.subscribeCrosshairMove(event => {
      if (updating.current || !event.sourceEvent) return;
      const next =
        event.time !== undefined
          ? (latest.current.series.points[Number(event.time) - 1]?.timestamp_ns ?? null)
          : null;
      setFocus(next);
      latest.current.onFocusChange?.(next);
    });
    const held = layers.current;
    return () => {
      resizeObserver?.disconnect();
      for (const { series: owner, line: item } of drawnLines.current) owner.removePriceLine(item);
      drawnLines.current = [];
      if (verticalPrimitive.current) {
        api.panes()[0]?.detachPrimitive(verticalPrimitive.current);
        verticalPrimitive.current = null;
      }
      if (extremesPrimitive.current) {
        api.panes()[0]?.detachPrimitive(extremesPrimitive.current);
        extremesPrimitive.current = null;
      }
      markers.current?.detach();
      markers.current = null;
      userLines.current = [];
      primarySeriesRef.current = null;
      api.remove();
      chart.current = null;
      held.clear();
    };
  }, [recordRange]);

  const referenceValue = referencePrice?.value;
  const referenceLabel = referencePrice?.label;
  useLayoutEffect(() => {
    const api = chart.current;
    if (!api) return;
    updating.current = true;
    for (const { series: owner, line: item } of drawnLines.current) owner.removePriceLine(item);
    drawnLines.current = [];
    const css = getComputedStyle(container.current!);
    const color = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const strokeOf = (value: string) =>
      value.startsWith("var(") ? color(value.slice(4, -1).trim(), "#86aaff") : value;
    const up = color("--quote-up", "#e45a65"),
      down = color("--quote-down", "#45b997");
    const price = color("--price-line", "#86aaff");
    const reference = referenceValue !== undefined ? Number(referenceValue) : null;
    const priceScaleId = reference === null ? "right" : "left";
    const format = props.formatPrice ?? numberText;
    api.applyOptions({
      timeScale: {
        minBarSpacing: series.kind === "candles" ? 1 : 0.0001,
      },
      leftPriceScale: {
        visible: reference !== null,
        borderVisible: false,
        // Room for the highest and lowest price labels.
        scaleMargins: { top: 0.08, bottom: 0.08 },
      },
      rightPriceScale: {
        visible: true,
        borderVisible: false,
        scaleMargins: { top: 0.08, bottom: 0.08 },
      },
    });
    const used = new Set<string>();
    const layer = (
      key: string,
      kind: "line" | "area" | "baseline" | "candle" | "histogram",
      pane: number,
    ) => {
      used.add(key);
      let entry = layers.current.get(key);
      if (!entry) {
        entry =
          kind === "candle"
            ? api.addSeries(CandlestickSeries, {}, pane)
            : kind === "histogram"
              ? api.addSeries(HistogramSeries, {}, pane)
              : kind === "baseline"
                ? api.addSeries(BaselineSeries, {}, pane)
                : kind === "area"
                  ? api.addSeries(AreaSeries, {}, pane)
                  : api.addSeries(LineSeries, {}, pane);
        layers.current.set(key, entry);
      }
      entry.moveToPane(pane);
      entry.applyOptions({ lastValueVisible: false, priceLineVisible: false });
      return entry;
    };
    const tick = priceTick ? Number(priceTick) : null;
    const shared = (): { priceRange: { minValue: number; maxValue: number } } | null => {
      // Both price axes use the same visible candles and overlays. Scaling
      // the mirror from close alone would misalign wicks and percentages.
      const range = api.timeScale().getVisibleLogicalRange();
      const first = range ? Math.max(0, Math.floor(range.from)) : 0;
      const last = range ? Math.min(count, Math.ceil(range.to) + 1) : count;
      let low = reference ?? Infinity,
        high = reference ?? -Infinity;
      const include = (value: number) => {
        low = Math.min(low, value);
        high = Math.max(high, value);
      };
      for (let i = first; i < last; i++) {
        const point = points[i];
        if ("high" in point) {
          include(Number(point.high));
          include(Number(point.low));
        } else if (valueAt(point) !== null) include(Number(valueAt(point)));
        for (const line of overlays) if (line.values[i] != null) include(line.values[i]!);
      }
      if (low === Infinity) return null;
      if (reference !== null) {
        const radius = Math.max(
          Math.abs(low - reference),
          Math.abs(high - reference),
          Math.abs(reference) * 0.001,
        );
        low = reference - radius;
        high = reference + radius;
      }
      return { priceRange: { minValue: low, maxValue: high } };
    };
    const priceOptions = {
      priceScaleId,
      priceFormat: { type: "custom" as const, minMove: tick ?? 0.00000001, formatter: format },
      autoscaleInfoProvider: (original: () => AutoscaleInfo | null) =>
        minimumTickSpan(reference === null ? original() : shared(), tick),
    };
    const timeline = layer("timeline", "line", 0);
    timeline.setData(points.map((_, i) => ({ time: time(i) })));
    timeline.applyOptions({ ...priceOptions, visible: false });
    const lines = (
      key: string,
      values: readonly (number | null)[],
      stroke: string,
      pane: number,
      area = false,
    ) => {
      let first: ISeriesApi<SeriesType> | undefined;
      chartRuns(values).forEach((run, i) => {
        const entry = layer(`${key}:${area}:${i}`, area ? "area" : "line", pane);
        first ??= entry;
        entry.applyOptions({
          ...(pane === 0 ? priceOptions : { priceScaleId: "right" }),
          color: stroke,
          lineColor: stroke,
          lineWidth: 1,
          pointMarkersVisible: run.length === 1,
          topColor: color("--chart-area", "#5278ce26"),
          bottomColor: "transparent",
        });
        entry.setData(run);
      });
      return first;
    };
    let primarySeries: ISeriesApi<SeriesType> | undefined;
    const useBaseline =
      series.kind === "line" && baselineValue !== undefined && Number.isFinite(baselineValue);
    if (series.kind === "candles") {
      const entry = layer("candles", "candle", 0);
      primarySeries = entry;
      entry.applyOptions({
        ...priceOptions,
        upColor: color("--canvas", "#0f1115"),
        downColor: down,
        borderUpColor: up,
        borderDownColor: down,
        wickUpColor: up,
        wickDownColor: down,
      });
      entry.setData(
        series.points.map((p, i) => ({
          time: time(i),
          open: Number(p.open),
          high: Number(p.high),
          low: Number(p.low),
          close: Number(p.close),
        })),
      );
    } else if (series.kind === "histogram") {
      const entry = layer("bars", "histogram", 0);
      primarySeries = entry;
      entry.applyOptions({ ...priceOptions, base: 0 });
      entry.setData(
        points.map((p, i) => {
          const value = valueAt(p);
          if (value === null) return { time: time(i) };
          const numeric = Number(value);
          return {
            time: time(i),
            value: numeric,
            color: numeric >= 0 ? withAlpha(up, "cc") : withAlpha(down, "cc"),
          };
        }),
      );
      drawnLines.current.push({
        series: entry,
        line: entry.createPriceLine({
          price: 0,
          color: color("--line-strong", "#384152"),
          lineWidth: 1,
          lineStyle: LineStyle.Dashed,
          axisLabelVisible: false,
        }),
      });
    } else if (useBaseline) {
      chartRuns(
        points.map(p => {
          const value = valueAt(p);
          return value === null ? null : Number(value);
        }),
      ).forEach((run, i) => {
        const entry = layer(`price:baseline:${i}`, "baseline", 0);
        primarySeries ??= entry;
        entry.applyOptions({
          ...priceOptions,
          baseValue: { type: "price", price: baselineValue },
          topLineColor: up,
          topFillColor1: fillArea ? withAlpha(up, "38") : "transparent",
          topFillColor2: fillArea ? withAlpha(up, "05") : "transparent",
          bottomLineColor: down,
          bottomFillColor1: fillArea ? withAlpha(down, "05") : "transparent",
          bottomFillColor2: fillArea ? withAlpha(down, "38") : "transparent",
          lineWidth: compact ? 1 : 2,
          pointMarkersVisible: run.length === 1,
        });
        entry.setData(run);
      });
    } else {
      primarySeries = lines(
        "price",
        points.map(p => {
          const value = valueAt(p);
          return value === null ? null : Number(value);
        }),
        price,
        0,
        fillArea,
      );
    }
    overlays.forEach(line => lines(`overlay:${line.label}`, line.values, strokeOf(line.color), 0));
    if (reference !== null) {
      const mirror = layer("mirror", "line", 0);
      mirror.applyOptions({
        ...priceOptions,
        priceScaleId: "right",
        lineVisible: false,
        crosshairMarkerVisible: false,
        priceFormat: {
          type: "custom",
          minMove: 0.00000001,
          formatter:
            reference <= 0
              ? format
              : (value: number) => `${(((value - reference) / reference) * 100).toFixed(2)}%`,
        },
      });
      mirror.setData(
        points.map((p, i) =>
          valueAt(p) === null ? { time: time(i) } : { time: time(i), value: Number(valueAt(p)) },
        ),
      );
    }
    if (
      primarySeries &&
      referenceValue !== undefined &&
      reference !== null &&
      Number.isFinite(reference)
    ) {
      drawnLines.current.push({
        series: primarySeries,
        line: primarySeries.createPriceLine({
          price: reference,
          color: color("--muted", "#999faa"),
          lineWidth: 1,
          lineStyle: LineStyle.Dashed,
          axisLabelVisible: false,
          title: referenceLabel ?? "",
        }),
      });
    }
    primarySeriesRef.current = primarySeries ?? null;
    if (showVolume) {
      const volume = layer("volume", "histogram", 1);
      volume.applyOptions({
        priceScaleId: "right",
        priceFormat: {
          type: "custom",
          minMove: 1,
          formatter: (value: number) =>
            value >= 1e6
              ? `${(value / 1e6).toFixed(1)}M`
              : value >= 1e3
                ? `${Math.round(value / 1e3)}K`
                : String(Math.round(value)),
        },
      });
      // Room above the tallest bar for the pane's label.
      volume.priceScale().applyOptions({ scaleMargins: { top: 0.25, bottom: 0 } });
      volume.setData(
        points.map((p, i) => {
          const previous = i > 0 ? valueAt(points[i - 1]) : null;
          const rising =
            "open" in p
              ? Number(p.close) >= Number(p.open)
              : previous === null || Number(valueAt(p)) >= Number(previous);
          return p.volume === undefined
            ? { time: time(i) }
            : { time: time(i), value: Number(p.volume), color: rising ? up : down };
        }),
      );
      if (points.some(p => p.openInterest !== undefined && p.openInterest !== "")) {
        const oiColor = color("--series-3", "#f0b849");
        chartRuns(
          points.map(p =>
            p.openInterest !== undefined && p.openInterest !== "" ? Number(p.openInterest) : null,
          ),
        ).forEach((run, i) => {
          const oi = layer(`oi:${i}`, "line", 1);
          oi.applyOptions({
            priceScaleId: "oi",
            color: oiColor,
            lineWidth: 1,
            crosshairMarkerVisible: false,
            pointMarkersVisible: run.length === 1,
          });
          oi.priceScale().applyOptions({
            visible: false,
            scaleMargins: { top: 0.28, bottom: 0.06 },
          });
          oi.setData(run);
        });
      }
    }
    if (oscillator) {
      const pane = showVolume ? 2 : 1;
      const histogram = layer("oscillator", "histogram", pane);
      histogram.setData(
        oscillator.histogram.map((value, i) =>
          value === null
            ? { time: time(i) }
            : { time: time(i), value, color: value >= 0 ? up : down },
        ),
      );
      oscillator.lines.forEach(line =>
        lines(`oscillator:${line.label}`, line.values, strokeOf(line.color), pane),
      );
    }
    for (const [key, entry] of layers.current)
      if (!used.has(key)) {
        if (key === "candles") {
          if (extremesPrimitive.current) {
            api.panes()[0]?.detachPrimitive(extremesPrimitive.current);
            extremesPrimitive.current = null;
          }
          markers.current?.detach();
          markers.current = null;
        }
        userLines.current = userLines.current.filter(({ series: owner }) => owner !== entry);
        api.removeSeries(entry);
        layers.current.delete(key);
      }
    api.panes().forEach((pane, i) => pane.setStretchFactor(i === 0 ? 3 : 1));
    const remembered = saved.current;
    const autoWidth = defaultVisibleCount(series.kind, count, container.current?.clientWidth ?? 0);
    const width = remembered.custom ? Math.min(count, remembered.size) : autoWidth;
    const at =
      remembered.anchor === null
        ? count - 1
        : points.findIndex(p => p.timestamp_ns === remembered.anchor);
    const right = Math.min(count, Math.max(width, at < 0 ? width : at + 1));
    const first = Math.max(0, right - width);
    if (count) api.timeScale().setVisibleLogicalRange({ from: first, to: right - 1 });
    updating.current = false;
    recordRange(first, right, remembered.custom);
  }, [
    series,
    overlays,
    oscillator,
    showVolume,
    referenceValue,
    referenceLabel,
    fillArea,
    baselineValue,
    compact,
    priceTick,
    props.formatPrice,
    count,
    points,
    recordRange,
    quoteColors,
    domQuoteColors,
  ]);

  useLayoutEffect(() => {
    for (const { series: owner, line: item } of userLines.current) owner.removePriceLine(item);
    userLines.current = [];
    const target = primarySeriesRef.current;
    if (!target || !priceLines.length || !container.current) return;
    const css = getComputedStyle(container.current);
    const color = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const up = color("--quote-up", "#e45a65"),
      down = color("--quote-down", "#45b997");
    for (const item of priceLines) {
      const numeric = Number(item.value);
      if (!Number.isFinite(numeric)) continue;
      const lineColor =
        item.tone === "up"
          ? up
          : item.tone === "down"
            ? down
            : item.tone === "accent"
              ? color("--accent", "#38d6f2")
              : color("--muted", "#999faa");
      userLines.current.push({
        series: target,
        line: target.createPriceLine({
          price: numeric,
          color: lineColor,
          lineWidth: 1,
          lineStyle: item.dashed === false ? LineStyle.Solid : LineStyle.Dashed,
          axisLabelVisible: item.axisLabelVisible ?? true,
          title: item.label,
        }),
      });
    }
  }, [priceLines, series, fillArea, baselineValue, quoteColors, domQuoteColors]);

  useLayoutEffect(() => {
    const api = chart.current;
    const pane = api?.panes()[0];
    if (!api || !pane) return;
    if (verticalPrimitive.current) {
      pane.detachPrimitive(verticalPrimitive.current);
      verticalPrimitive.current = null;
    }
    const valid = verticalMarkers.filter(item => item.index >= 0 && item.index < count);
    if (!valid.length) return;
    const css = getComputedStyle(container.current ?? document.documentElement);
    const color = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const stroke = withAlpha(color("--accent", "#38d6f2"), "aa");
    const fill = withAlpha(color("--raised", "#111720"), "ee");
    const text = color("--text-strong", "#eef2f8");
    const primitive = verticalLinePrimitive(api, valid, stroke, fill, text);
    pane.attachPrimitive(primitive);
    verticalPrimitive.current = primitive;
  }, [verticalMarkers, count, quoteColors, domQuoteColors]);

  useLayoutEffect(() => {
    const api = chart.current;
    const pane = api?.panes()[0];
    const candles = layers.current.get("candles");
    if (pane && extremesPrimitive.current) {
      pane.detachPrimitive(extremesPrimitive.current);
      extremesPrimitive.current = null;
    }
    if (!api || !pane || !candles || series.kind !== "candles") return;
    if (!markers.current) markers.current = createSeriesMarkers(candles, []);
    const css = getComputedStyle(container.current ?? document.documentElement);
    const color = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const traded = trades
      .filter(trade => trade.index >= 0 && trade.index < count)
      .map(trade => ({
        time: time(trade.index),
        position: trade.side === "buy" ? ("belowBar" as const) : ("aboveBar" as const),
        color:
          trade.side === "buy" ? color("--quote-up", "#e45a65") : color("--quote-down", "#45b997"),
        shape: trade.side === "buy" ? ("arrowUp" as const) : ("arrowDown" as const),
        text: trade.text,
      }))
      .sort((a, b) => Number(a.time) - Number(b.time));
    markers.current.setMarkers(traded);
    if (!showPriceExtremes || !count) return;
    const visible = series.points.slice(start, end);
    const highAt = visible.reduce(
      (at, p, i) => (Number(p.high) > Number(visible[at].high) ? i : at),
      0,
    );
    const lowAt = visible.reduce(
      (at, p, i) => (Number(p.low) < Number(visible[at].low) ? i : at),
      0,
    );
    const items =
      visible[highAt].high === visible[lowAt].low
        ? [{ index: start + highAt, price: visible[highAt].high }]
        : [
            { index: start + highAt, price: visible[highAt].high },
            { index: start + lowAt, price: visible[lowAt].low },
          ];
    const fontFamily = color("--font-terminal", "ui-monospace, SFMono-Regular, Menlo, monospace");
    const primitive = priceExtremesPrimitive(
      api,
      candles,
      items,
      color("--muted", "#999faa"),
      fontFamily,
      Number.parseFloat(color("--font-size-xs", "11")),
    );
    pane.attachPrimitive(primitive);
    extremesPrimitive.current = primitive;
  }, [series, showPriceExtremes, trades, start, end, count, quoteColors, domQuoteColors]);

  useLayoutEffect(() => {
    const api = chart.current;
    if (!api) return;
    let main = layers.current.get("candles") ?? layers.current.get("bars");
    if (!main && selected >= 0 && series.kind === "line") {
      const runs = chartRuns(
        points.map(p => {
          const value = valueAt(p);
          return value === null ? null : Number(value);
        }),
      );
      const targetTime = time(selected);
      const runIndex = Math.max(
        0,
        runs.findIndex(run => run.some(item => item.time === targetTime)),
      );
      const useBaseline = baselineValue !== undefined && Number.isFinite(baselineValue);
      main = layers.current.get(
        useBaseline ? `price:baseline:${runIndex}` : `price:${fillArea}:${runIndex}`,
      );
    }
    if (selected < 0 || !main || valueAt(points[selected]) === null) api.clearCrosshairPosition();
    else api.setCrosshairPosition(Number(valueAt(points[selected])), time(selected), main);
  }, [selected, points, series.kind, fillArea, baselineValue]);

  const showRange = (first: number, last: number) => {
    // Remember navigation synchronously; the engine applies its range on the
    // next animation frame. Rapid key presses must compose before that frame.
    recordRange(first, last, true);
    chart.current?.timeScale().setVisibleLogicalRange({ from: first, to: last - 1 });
  };
  const pan = (right: number) => {
    const next = Math.max(size, Math.min(count, right));
    showRange(next - size, next);
    updateFocus(null);
  };
  const zoom = (factor: number) => {
    const width = Math.max(Math.min(5, count), Math.min(count, Math.round(size * factor)));
    const right = Math.max(width, end);
    showRange(right - width, right);
  };
  const last = () => {
    pan(count);
    updateFocus(null);
  };
  return (
    <div className="price-chart" data-fit-height={fitHeight || undefined}>
      <p className="price-chart-cursor" aria-live="polite">
        {titled && <strong>{label}</strong>}
        {count ? describe(index) : null}
      </p>
      {!!overlays.length && (
        <div className="price-chart-legend">
          {overlays.map(line => (
            <span data-indicator={line.label} key={line.label} style={{ color: line.color }}>
              {line.label}:{" "}
              {line.values[index] == null ? "—" : formatIndicatorValue(line.values[index]!)}
            </span>
          ))}
        </div>
      )}
      {controls && (
        <div className="price-chart-controls">
          <button
            aria-label={controls.earlier}
            disabled={start === 0}
            onClick={() => pan(end - Math.max(1, Math.round(size / 5)))}
          >
            <Icon name="chevronLeft" size={14} />
          </button>
          <button
            aria-label={controls.zoomIn}
            disabled={size <= Math.min(5, count)}
            onClick={() => zoom(0.8)}
          >
            <Icon name="plus" size={14} />
          </button>
          <button
            aria-label={controls.zoomOut}
            disabled={size === count}
            onClick={() => zoom(1.25)}
          >
            <Icon name="minus" size={14} />
          </button>
          <button
            aria-label={controls.later}
            disabled={end === count}
            onClick={() => pan(end + Math.max(1, Math.round(size / 5)))}
          >
            <Icon name="chevronRight" size={14} />
          </button>
          <button onClick={last}>{controls.latest}</button>
        </div>
      )}
      <div
        className="price-chart-canvas"
        style={{
          height: fitHeight
            ? undefined
            : `var(--chart-height${compact ? "-compact" : oscillator ? "-oscillator" : ""})`,
        }}
        ref={container}
        role="img"
        aria-label={label}
        tabIndex={0}
        data-visible-count={size}
        data-window-start={start}
        onPointerLeave={() => updateFocus(null)}
        onKeyDown={event => {
          if (!count || event.metaKey || event.ctrlKey || event.altKey) return;
          if (["ArrowLeft", "ArrowRight", "+", "=", "-", "Home", "End"].includes(event.key))
            event.preventDefault();
          if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
            const direction = event.key === "ArrowLeft" ? -1 : 1;
            if (event.shiftKey) pan(end + direction * Math.max(1, Math.round(size / 5)));
            else {
              const next = Math.max(start, Math.min(end - 1, index + direction));
              updateFocus(points[next].timestamp_ns);
            }
          } else if (event.key === "+" || event.key === "=") zoom(0.8);
          else if (event.key === "-") zoom(1.25);
          else if (event.key === "Home") pan(size);
          else if (event.key === "End") last();
        }}
      />
      {referencePrice && (
        <div className="price-chart-legend" data-chart-reference={referencePrice.value}>
          {referencePrice.label}: {referencePrice.value}
        </div>
      )}
      {showVolume && volumeAt && (
        <div
          className="price-chart-pane-label"
          data-chart-volume-axis
          style={{ top: volumeAt.top, left: volumeAt.left }}
        >
          {volumeLabel} {points[index]?.volume ?? "—"}
          {openInterestLabel && points[index]?.openInterest !== undefined && (
            <>
              {" · "}
              <span className="price-chart-oi-label">
                {openInterestLabel} {points[index]?.openInterest || "—"}
              </span>
            </>
          )}
        </div>
      )}
      {oscillator && (
        <div className="price-chart-legend" data-oscillator={oscillator.label}>
          <span data-oscillator-legend>
            {oscillator.label}{" "}
            {oscillator.lines.map(line => (
              <span key={line.label} style={{ color: line.color }}>
                {line.label}:{" "}
                {line.values[index] == null ? "—" : formatIndicatorValue(line.values[index]!)}{" "}
              </span>
            ))}
            {oscillator.histogramLabel ?? oscillator.label}:{" "}
            {oscillator.histogram[index] == null
              ? "—"
              : formatIndicatorValue(oscillator.histogram[index]!)}
          </span>
          {!oscillator.histogram.slice(start, end).some(value => value !== null) &&
            !oscillator.lines.some(line =>
              line.values.slice(start, end).some(value => value !== null),
            ) && <span data-oscillator-empty>{oscillator.emptyLabel}</span>}
        </div>
      )}
    </div>
  );
}
