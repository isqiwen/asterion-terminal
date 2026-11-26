import {
  AreaSeries,
  BaselineSeries,
  CandlestickSeries,
  HistogramSeries,
  LineSeries,
  LineStyle,
  createSeriesMarkers,
  type AutoscaleInfo,
  type IChartApi,
  type IPanePrimitive,
  type IPriceLine,
  type ISeriesApi,
  type ISeriesMarkersPluginApi,
  type SeriesType,
  type Time,
} from "lightweight-charts";
import {
  chartRuns,
  chartTime,
  cssColor,
  defaultVisibleCount,
  minimumTickSpan,
  numberText,
  pointValue,
  visiblePriceExtremes,
  withAlpha,
  type ChartLine,
  type ChartPriceLine,
  type ChartSeries,
  type OscillatorSpec,
  type TradeMark,
  type VerticalMarker,
} from "./price-chart-math";
import { priceExtremesPrimitive, verticalLinePrimitive } from "./price-chart-primitives";

export type ChartRefs = {
  drawnLines: { series: ISeriesApi<SeriesType>; line: IPriceLine }[];
  userLines: { series: ISeriesApi<SeriesType>; line: IPriceLine }[];
  primarySeries: ISeriesApi<SeriesType> | null;
  markers: ISeriesMarkersPluginApi<Time> | null;
  verticalPrimitive: IPanePrimitive<Time> | null;
  extremesPrimitive: IPanePrimitive<Time> | null;
  layers: Map<string, ISeriesApi<SeriesType>>;
  saved: {
    start: number;
    end: number;
    anchor: string | null;
    size: number;
    custom: boolean;
  };
  updating: boolean;
};

export function createChartRefs(): ChartRefs {
  return {
    drawnLines: [],
    userLines: [],
    primarySeries: null,
    markers: null,
    verticalPrimitive: null,
    extremesPrimitive: null,
    layers: new Map(),
    saved: { start: 0, end: 0, anchor: null, size: 0, custom: false },
    updating: false,
  };
}

export function disposeChartRefs(api: IChartApi, refs: ChartRefs) {
  for (const { series: owner, line: item } of refs.drawnLines) owner.removePriceLine(item);
  refs.drawnLines = [];
  if (refs.verticalPrimitive) {
    api.panes()[0]?.detachPrimitive(refs.verticalPrimitive);
    refs.verticalPrimitive = null;
  }
  if (refs.extremesPrimitive) {
    api.panes()[0]?.detachPrimitive(refs.extremesPrimitive);
    refs.extremesPrimitive = null;
  }
  refs.markers?.detach();
  refs.markers = null;
  refs.userLines = [];
  refs.primarySeries = null;
  refs.layers.clear();
}

export type SyncSeriesOptions = {
  series: ChartSeries;
  overlays: readonly ChartLine[];
  oscillator?: OscillatorSpec;
  showVolume: boolean;
  referenceValue?: string;
  referenceLabel?: string;
  fillArea: boolean;
  baselineValue?: number;
  compact: boolean;
  priceTick?: string;
  formatPrice?: (value: number) => string;
  recordRange: (first: number, last: number, custom: boolean) => void;
};

export function syncChartSeries(
  api: IChartApi,
  element: HTMLElement,
  refs: ChartRefs,
  opts: SyncSeriesOptions,
) {
  const {
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
    formatPrice,
    recordRange,
  } = opts;
  const points = series.points;
  const count = points.length;

  refs.updating = true;
  for (const { series: owner, line: item } of refs.drawnLines) owner.removePriceLine(item);
  refs.drawnLines = [];

  const css = getComputedStyle(element);
  const color = (name: string, fallback: string) => cssColor(css, name, fallback);
  const strokeOf = (value: string) =>
    value.startsWith("var(") ? color(value.slice(4, -1).trim(), "#86aaff") : value;
  const up = color("--quote-up", "#e45a65");
  const down = color("--quote-down", "#45b997");
  const price = color("--price-line", "#86aaff");
  const reference = referenceValue !== undefined ? Number(referenceValue) : null;
  const priceScaleId = reference === null ? "right" : "left";
  const format = formatPrice ?? numberText;

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
    let entry = refs.layers.get(key);
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
      refs.layers.set(key, entry);
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
    let low = reference ?? Infinity;
    let high = reference ?? -Infinity;
    const include = (value: number) => {
      low = Math.min(low, value);
      high = Math.max(high, value);
    };
    for (let i = first; i < last; i++) {
      const point = points[i];
      if ("high" in point) {
        include(Number(point.high));
        include(Number(point.low));
      } else if (pointValue(point) !== null) include(Number(pointValue(point)));
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
  timeline.setData(points.map((_, i) => ({ time: chartTime(i) })));
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
        time: chartTime(i),
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
        const value = pointValue(p);
        if (value === null) return { time: chartTime(i) };
        const numeric = Number(value);
        return {
          time: chartTime(i),
          value: numeric,
          color: numeric >= 0 ? withAlpha(up, "cc") : withAlpha(down, "cc"),
        };
      }),
    );
    refs.drawnLines.push({
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
        const value = pointValue(p);
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
        const value = pointValue(p);
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
        pointValue(p) === null
          ? { time: chartTime(i) }
          : { time: chartTime(i), value: Number(pointValue(p)) },
      ),
    );
  }

  if (
    primarySeries &&
    referenceValue !== undefined &&
    reference !== null &&
    Number.isFinite(reference)
  ) {
    refs.drawnLines.push({
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
  refs.primarySeries = primarySeries ?? null;

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
        const previous = i > 0 ? pointValue(points[i - 1]) : null;
        const rising =
          "open" in p
            ? Number(p.close) >= Number(p.open)
            : previous === null || Number(pointValue(p)) >= Number(previous);
        return p.volume === undefined
          ? { time: chartTime(i) }
          : { time: chartTime(i), value: Number(p.volume), color: rising ? up : down };
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
    histogram.applyOptions({ priceScaleId: "right" });
    // Room above the pane's highest value for its top-left label.
    histogram.priceScale().applyOptions({ scaleMargins: { top: 0.25, bottom: 0.06 } });
    histogram.setData(
      oscillator.histogram.map((value, i) =>
        value === null
          ? { time: chartTime(i) }
          : { time: chartTime(i), value, color: value >= 0 ? up : down },
      ),
    );
    oscillator.lines.forEach(line =>
      lines(`oscillator:${line.label}`, line.values, strokeOf(line.color), pane),
    );
  }

  for (const [key, entry] of refs.layers) {
    if (!used.has(key)) {
      if (key === "candles") {
        if (refs.extremesPrimitive) {
          api.panes()[0]?.detachPrimitive(refs.extremesPrimitive);
          refs.extremesPrimitive = null;
        }
        refs.markers?.detach();
        refs.markers = null;
      }
      refs.userLines = refs.userLines.filter(({ series: owner }) => owner !== entry);
      api.removeSeries(entry);
      refs.layers.delete(key);
    }
  }

  api.panes().forEach((pane, i) => pane.setStretchFactor(i === 0 ? 3 : 1));
  const remembered = refs.saved;
  const autoWidth = defaultVisibleCount(series.kind, count, element.clientWidth);
  const width = remembered.custom ? Math.min(count, remembered.size) : autoWidth;
  const at =
    remembered.anchor === null
      ? count - 1
      : points.findIndex(p => p.timestamp_ns === remembered.anchor);
  const right = Math.min(count, Math.max(width, at < 0 ? width : at + 1));
  const first = Math.max(0, right - width);
  if (count) api.timeScale().setVisibleLogicalRange({ from: first, to: right - 1 });
  refs.updating = false;
  recordRange(first, right, remembered.custom);
}

export function syncUserPriceLines(
  element: HTMLElement | null,
  refs: ChartRefs,
  priceLines: readonly ChartPriceLine[],
) {
  for (const { series: owner, line: item } of refs.userLines) owner.removePriceLine(item);
  refs.userLines = [];
  const target = refs.primarySeries;
  if (!target || !priceLines.length || !element) return;
  const css = getComputedStyle(element);
  const up = cssColor(css, "--quote-up", "#e45a65");
  const down = cssColor(css, "--quote-down", "#45b997");
  for (const item of priceLines) {
    const numeric = Number(item.value);
    if (!Number.isFinite(numeric)) continue;
    const lineColor =
      item.tone === "up"
        ? up
        : item.tone === "down"
          ? down
          : item.tone === "accent"
            ? cssColor(css, "--accent", "#38d6f2")
            : cssColor(css, "--muted", "#999faa");
    refs.userLines.push({
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
}

export function syncVerticalMarkers(
  api: IChartApi | null,
  element: HTMLElement | null,
  refs: ChartRefs,
  verticalMarkers: readonly VerticalMarker[],
  count: number,
) {
  const pane = api?.panes()[0];
  if (!api || !pane) return;
  if (refs.verticalPrimitive) {
    pane.detachPrimitive(refs.verticalPrimitive);
    refs.verticalPrimitive = null;
  }
  const valid = verticalMarkers.filter(item => item.index >= 0 && item.index < count);
  if (!valid.length) return;
  const css = getComputedStyle(element ?? document.documentElement);
  const stroke = withAlpha(cssColor(css, "--accent", "#38d6f2"), "aa");
  const fill = withAlpha(cssColor(css, "--raised", "#111720"), "ee");
  const text = cssColor(css, "--text-strong", "#eef2f8");
  const primitive = verticalLinePrimitive(api, valid, stroke, fill, text);
  pane.attachPrimitive(primitive);
  refs.verticalPrimitive = primitive;
}

export function syncTradeMarkersAndExtremes(
  api: IChartApi | null,
  element: HTMLElement | null,
  refs: ChartRefs,
  series: ChartSeries,
  trades: readonly TradeMark[],
  showPriceExtremes: boolean,
  start: number,
  end: number,
) {
  const pane = api?.panes()[0];
  const candles = refs.layers.get("candles");
  if (pane && refs.extremesPrimitive) {
    pane.detachPrimitive(refs.extremesPrimitive);
    refs.extremesPrimitive = null;
  }
  if (!api || !pane || !candles || series.kind !== "candles") return;
  const count = series.points.length;
  if (!refs.markers) refs.markers = createSeriesMarkers(candles, []);
  const css = getComputedStyle(element ?? document.documentElement);
  const traded = trades
    .filter(trade => trade.index >= 0 && trade.index < count)
    .map(trade => ({
      time: chartTime(trade.index),
      position: trade.side === "buy" ? ("belowBar" as const) : ("aboveBar" as const),
      color:
        trade.side === "buy"
          ? cssColor(css, "--quote-up", "#e45a65")
          : cssColor(css, "--quote-down", "#45b997"),
      shape: trade.side === "buy" ? ("arrowUp" as const) : ("arrowDown" as const),
      text: trade.text,
    }))
    .sort((a, b) => Number(a.time) - Number(b.time));
  refs.markers.setMarkers(traded);
  if (!showPriceExtremes || !count) return;
  const items = visiblePriceExtremes(series.points, start, end);
  if (!items.length) return;
  const fontFamily = cssColor(
    css,
    "--font-terminal",
    "ui-monospace, SFMono-Regular, Menlo, monospace",
  );
  const primitive = priceExtremesPrimitive(
    api,
    candles,
    items,
    cssColor(css, "--muted", "#999faa"),
    fontFamily,
    Number.parseFloat(cssColor(css, "--font-size-xs", "11")),
  );
  pane.attachPrimitive(primitive);
  refs.extremesPrimitive = primitive;
}

export function syncCrosshair(
  api: IChartApi | null,
  refs: ChartRefs,
  series: ChartSeries,
  selected: number,
  fillArea: boolean,
  baselineValue?: number,
) {
  if (!api) return;
  const points = series.points;
  let main = refs.layers.get("candles") ?? refs.layers.get("bars");
  if (!main && selected >= 0 && series.kind === "line") {
    const runs = chartRuns(
      points.map(p => {
        const value = pointValue(p);
        return value === null ? null : Number(value);
      }),
    );
    const targetTime = chartTime(selected);
    const runIndex = Math.max(
      0,
      runs.findIndex(run => run.some(item => item.time === targetTime)),
    );
    const useBaseline = baselineValue !== undefined && Number.isFinite(baselineValue);
    main = refs.layers.get(
      useBaseline ? `price:baseline:${runIndex}` : `price:${fillArea}:${runIndex}`,
    );
  }
  if (selected < 0 || !main || pointValue(points[selected]) === null) api.clearCrosshairPosition();
  else api.setCrosshairPosition(Number(pointValue(points[selected])), chartTime(selected), main);
}
