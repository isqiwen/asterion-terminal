import { useCallback, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import {
  AreaSeries,
  CandlestickSeries,
  ColorType,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  createChart,
  createSeriesMarkers,
  type IChartApi,
  type ISeriesApi,
  type ISeriesMarkersPluginApi,
  type SeriesType,
  type Time,
  type UTCTimestamp,
  type LogicalRange,
} from "lightweight-charts";
import "./price-chart.css";

// Exact domain values and timestamps stay with providers. Ordinal chart times
// avoid rounding nanoseconds or inventing calendar minutes between observations.
export type PricePoint = { timestamp_ns: string; price: string | null; volume?: string };
export type Candle = {
  timestamp_ns: string;
  open: string;
  high: string;
  low: string;
  close: string;
  volume: string;
};
type Series =
  { kind: "line"; points: readonly PricePoint[] } | { kind: "candles"; points: readonly Candle[] };
type Line = { label: string; color: string; values: readonly (number | null)[] };
type Props = {
  series: Series;
  label: string;
  volumeLabel: string;
  formatTime: (value: string) => string;
  formatAxisTime?: (value: string) => string;
  describe: (index: number) => ReactNode;
  controls?: { zoomIn: string; zoomOut: string; earlier: string; later: string; latest: string };
  showVolume?: boolean;
  compact?: boolean;
  fillArea?: boolean;
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
  showPriceExtremes?: boolean;
  mirrorPriceAxis?: boolean;
  formatIndicatorValue?: (value: number) => string;
  formatPrice?: (value: number) => string;
};
const emptyLines: readonly Line[] = [];
const time = (index: number) => (index + 1) as UTCTimestamp;
const valueAt = (point: PricePoint | Candle) => ("close" in point ? point.close : point.price);
const numberText = (value: number) => Number(value.toPrecision(8)).toString();

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

export function PriceChart(props: Props) {
  const {
    series,
    label,
    volumeLabel,
    describe,
    controls,
    showVolume = true,
    compact = false,
    fillArea = false,
    fitHeight = false,
    overlays = emptyLines,
    oscillator,
    referencePrice,
    showPriceExtremes = false,
    mirrorPriceAxis = false,
    formatIndicatorValue = numberText,
  } = props;
  const container = useRef<HTMLDivElement>(null);
  const chart = useRef<IChartApi | null>(null);
  const markers = useRef<ISeriesMarkersPluginApi<Time> | null>(null);
  const layers = useRef(new Map<string, ISeriesApi<SeriesType>>());
  const latest = useRef(props);
  const saved = useRef<{ anchor: string | null; size: number; all: boolean }>({
    anchor: null,
    size: 0,
    all: true,
  });
  const updating = useRef(false);
  const [focus, setFocus] = useState<string | null>(null);
  const [view, setView] = useState({ start: 0, end: series.points.length });
  const points = series.points,
    count = points.length;
  const start = Math.min(view.start, Math.max(0, count - 1));
  const end = Math.max(start + 1, Math.min(view.end, count));
  const size = end - start;
  const selected = focus === null ? -1 : points.findIndex(p => p.timestamp_ns === focus);
  let lastObserved = end - 1;
  while (count && lastObserved > start && valueAt(points[lastObserved]) === null) lastObserved--;
  const index = Math.max(start, Math.min(end - 1, selected < 0 ? lastObserved : selected));

  const recordRange = useCallback((first: number, last: number) => {
    const p = latest.current.series.points;
    saved.current = {
      anchor: last === p.length || !last ? null : p[last - 1].timestamp_ns,
      size: last - first,
      all: first === 0 && last === p.length,
    };
    setView({ start: first, end: last });
  }, []);
  useLayoutEffect(() => {
    latest.current = props;
  });
  useLayoutEffect(() => {
    if (!container.current) return;
    const css = getComputedStyle(container.current);
    const text = css.getPropertyValue("--muted").trim() || "#999faa";
    const grid = css.getPropertyValue("--grid").trim() || "#23262c";
    const when = (at: Time, axis: boolean) => {
      const p = latest.current;
      const point = p.series.points[Number(at) - 1];
      return point
        ? (axis ? (p.formatAxisTime ?? p.formatTime) : p.formatTime)(point.timestamp_ns)
        : "";
    };
    const api = createChart(container.current, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: "transparent" },
        textColor: text,
        fontSize: 11,
        attributionLogo: false,
        panes: { separatorColor: grid, separatorHoverColor: grid },
      },
      grid: { vertLines: { color: grid }, horzLines: { color: grid } },
      crosshair: { mode: CrosshairMode.Normal },
      timeScale: {
        tickMarkFormatter: (t: Time) => when(t, true),
        rightOffset: 0,
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
      recordRange(first, last);
    };
    api.timeScale().subscribeVisibleLogicalRangeChange(rangeChanged);
    api.subscribeCrosshairMove(event => {
      if (updating.current || !event.sourceEvent) return;
      if (event.time !== undefined) {
        const point = latest.current.series.points[Number(event.time) - 1];
        if (point) setFocus(point.timestamp_ns);
      } else setFocus(null);
    });
    const held = layers.current;
    return () => {
      markers.current?.detach();
      markers.current = null;
      api.remove();
      chart.current = null;
      held.clear();
    };
  }, [recordRange]);

  useLayoutEffect(() => {
    const api = chart.current;
    if (!api) return;
    updating.current = true;
    const css = getComputedStyle(container.current!);
    const color = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const up = color("--quote-up", "#e45a65"),
      down = color("--quote-down", "#45b997");
    const price = color("--price-line", "#86aaff");
    const reference = referencePrice ? Number(referencePrice.value) : null;
    const priceScaleId = reference === null ? "right" : "left";
    const format = props.formatPrice ?? numberText;
    api.applyOptions({
      leftPriceScale: { visible: reference !== null || mirrorPriceAxis, borderVisible: false },
      rightPriceScale: { visible: true, borderVisible: false },
    });
    const used = new Set<string>();
    const layer = (key: string, kind: "line" | "area" | "candle" | "histogram", pane: number) => {
      used.add(key);
      let entry = layers.current.get(key);
      if (!entry) {
        entry =
          kind === "candle"
            ? api.addSeries(CandlestickSeries, {}, pane)
            : kind === "histogram"
              ? api.addSeries(HistogramSeries, {}, pane)
              : kind === "area"
                ? api.addSeries(AreaSeries, {}, pane)
                : api.addSeries(LineSeries, {}, pane);
        layers.current.set(key, entry);
      }
      entry.moveToPane(pane);
      entry.applyOptions({ lastValueVisible: false, priceLineVisible: false });
      return entry;
    };
    const priceOptions = {
      priceScaleId,
      priceFormat: { type: "custom" as const, minMove: 0.00000001, formatter: format },
      autoscaleInfoProvider: (
        original: () => { priceRange: { minValue: number; maxValue: number } } | null,
      ) => {
        if (reference === null && !mirrorPriceAxis) return original();
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
          } else if (point.price !== null) include(Number(point.price));
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
      },
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
      chartRuns(values).forEach((run, i) => {
        const entry = layer(`${key}:${area}:${i}`, area ? "area" : "line", pane);
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
    };
    if (series.kind === "candles") {
      const entry = layer("candles", "candle", 0);
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
    } else
      lines(
        "price",
        points.map(p => {
          const value = valueAt(p);
          return value === null ? null : Number(value);
        }),
        price,
        0,
        fillArea,
      );
    overlays.forEach(line => lines(`overlay:${line.label}`, line.values, line.color, 0));
    if (reference !== null || mirrorPriceAxis) {
      const mirror = layer("mirror", "line", 0);
      mirror.applyOptions({
        ...priceOptions,
        priceScaleId: reference === null ? "left" : "right",
        lineVisible: false,
        crosshairMarkerVisible: false,
        priceFormat: {
          type: "custom",
          minMove: 0.00000001,
          formatter:
            reference === null
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
    if (referencePrice) {
      const ref = layer("reference", "line", 0);
      ref.applyOptions({
        ...priceOptions,
        color: color("--muted", "#999faa"),
        lineWidth: 1,
        lineStyle: 2,
        crosshairMarkerVisible: false,
        title: referencePrice.label,
      });
      ref.setData(points.map((_, i) => ({ time: time(i), value: Number(referencePrice.value) })));
    }
    if (showVolume) {
      const volume = layer("volume", "histogram", 1);
      volume.applyOptions({ priceScaleId: "right", priceFormat: { type: "volume" } });
      volume.setData(
        points.map((p, i) => {
          const rising =
            "open" in p
              ? Number(p.close) >= Number(p.open)
              : i === 0 || Number(valueAt(p)) >= Number(valueAt(points[i - 1]));
          return p.volume === undefined
            ? { time: time(i) }
            : { time: time(i), value: Number(p.volume), color: rising ? up : down };
        }),
      );
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
        lines(`oscillator:${line.label}`, line.values, line.color, pane),
      );
    }
    for (const [key, entry] of layers.current)
      if (!used.has(key)) {
        if (key === "candles") {
          markers.current?.detach();
          markers.current = null;
        }
        api.removeSeries(entry);
        layers.current.delete(key);
      }
    api.panes().forEach((pane, i) => pane.setStretchFactor(i === 0 ? 3 : 1));
    const remembered = saved.current;
    const width = remembered.all ? count : Math.min(count, remembered.size);
    const at =
      remembered.anchor === null
        ? count - 1
        : points.findIndex(p => p.timestamp_ns === remembered.anchor);
    const right = Math.min(count, Math.max(width, at < 0 ? width : at + 1));
    if (count) api.timeScale().setVisibleLogicalRange({ from: right - width, to: right - 1 });
    updating.current = false;
    recordRange(Math.max(0, right - width), right);
  }, [
    series,
    overlays,
    oscillator,
    showVolume,
    referencePrice,
    mirrorPriceAxis,
    fillArea,
    props.formatPrice,
    count,
    points,
    recordRange,
  ]);

  useLayoutEffect(() => {
    const candles = layers.current.get("candles");
    if (!candles || series.kind !== "candles") return;
    if (!markers.current) markers.current = createSeriesMarkers(candles, []);
    if (!showPriceExtremes || !count) {
      markers.current.setMarkers([]);
      return;
    }
    const visible = series.points.slice(start, end);
    markers.current.setMarkers(
      (["high", "low"] as const)
        .map(field => {
          const best = visible.reduce(
            (at, p, i) =>
              (
                field === "high"
                  ? Number(p[field]) > Number(visible[at][field])
                  : Number(p[field]) < Number(visible[at][field])
              )
                ? i
                : at,
            0,
          );
          return {
            time: time(start + best),
            position: field === "high" ? ("aboveBar" as const) : ("belowBar" as const),
            color: "#999faa",
            shape: "circle" as const,
            text: visible[best][field],
            size: 0,
          };
        })
        .sort((a, b) => Number(a.time) - Number(b.time)),
    );
  }, [series, showPriceExtremes, start, end, count]);

  useLayoutEffect(() => {
    const api = chart.current;
    if (!api) return;
    const main = layers.current.get("candles") ?? layers.current.get(`price:${fillArea}:0`);
    if (selected < 0 || !main || valueAt(points[selected]) === null) api.clearCrosshairPosition();
    else api.setCrosshairPosition(Number(valueAt(points[selected])), time(selected), main);
  }, [selected, points, fillArea]);

  const showRange = (first: number, last: number) => {
    // Remember navigation synchronously; the engine applies its range on the
    // next animation frame. Rapid key presses must compose before that frame.
    recordRange(first, last);
    chart.current?.timeScale().setVisibleLogicalRange({ from: first, to: last - 1 });
  };
  const pan = (right: number) => {
    const next = Math.max(size, Math.min(count, right));
    showRange(next - size, next);
    setFocus(null);
  };
  const zoom = (factor: number) => {
    const width = Math.max(Math.min(5, count), Math.min(count, Math.round(size * factor)));
    const right = Math.max(width, end);
    showRange(right - width, right);
  };
  const last = () => {
    pan(count);
    setFocus(null);
  };
  return (
    <div className="price-chart" data-fit-height={fitHeight || undefined}>
      <p className="price-chart-cursor" aria-live="polite">
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
            ←
          </button>
          <button
            aria-label={controls.zoomIn}
            disabled={size <= Math.min(5, count)}
            onClick={() => zoom(0.8)}
          >
            +
          </button>
          <button
            aria-label={controls.zoomOut}
            disabled={size === count}
            onClick={() => zoom(1.25)}
          >
            −
          </button>
          <button
            aria-label={controls.later}
            disabled={end === count}
            onClick={() => pan(end + Math.max(1, Math.round(size / 5)))}
          >
            →
          </button>
          <button onClick={last}>{controls.latest}</button>
        </div>
      )}
      <div
        className="price-chart-canvas"
        style={{ height: fitHeight ? undefined : compact ? 240 : oscillator ? 430 : 340 }}
        ref={container}
        role="img"
        aria-label={label}
        tabIndex={0}
        data-visible-count={size}
        data-window-start={start}
        onPointerLeave={() => setFocus(null)}
        onKeyDown={event => {
          if (!count) return;
          if (["ArrowLeft", "ArrowRight", "+", "=", "-", "Home", "End"].includes(event.key))
            event.preventDefault();
          if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
            const direction = event.key === "ArrowLeft" ? -1 : 1;
            if (event.shiftKey) pan(end + direction * Math.max(1, Math.round(size / 5)));
            else {
              const next = Math.max(start, Math.min(end - 1, index + direction));
              setFocus(points[next].timestamp_ns);
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
      {showVolume && (
        <div className="price-chart-legend" data-chart-volume-axis>
          {volumeLabel}: {points[index]?.volume ?? "—"}
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
      <a
        className="chart-attribution"
        href="https://www.tradingview.com/"
        target="_blank"
        rel="noreferrer"
      >
        TradingView Lightweight Charts™ · © 2025 TradingView, Inc.
      </a>
    </div>
  );
}
