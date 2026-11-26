import { useCallback, useEffect, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import {
  ColorType,
  CrosshairMode,
  createChart,
  type IChartApi,
  type LogicalRange,
  type Time,
} from "lightweight-charts";
import { Icon } from "../ui/Icon";
import { usePreferences } from "../ui/preferences";
import {
  cssColor,
  defaultVisibleCount,
  numberText,
  pointValue,
  withAlpha,
  type Candle,
  type ChartLine,
  type ChartPriceLine,
  type ChartSeries,
  type OscillatorSpec,
  type PricePoint,
  type TradeMark,
  type VerticalMarker,
} from "./price-chart-math";
import {
  createChartRefs,
  disposeChartRefs,
  syncChartSeries,
  syncCrosshair,
  syncTradeMarkersAndExtremes,
  syncUserPriceLines,
  syncVerticalMarkers,
} from "./price-chart-series";
import "./price-chart.css";

export type { Candle, ChartPriceLine, PricePoint, TradeMark, VerticalMarker };

type Props = {
  series: ChartSeries;
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
  overlays?: readonly ChartLine[];
  oscillator?: OscillatorSpec;
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

const emptyLines: readonly ChartLine[] = [];
const emptyPriceLines: readonly ChartPriceLine[] = [];
const emptyVerticalMarkers: readonly VerticalMarker[] = [];
const noTrades: readonly TradeMark[] = [];

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
  const refs = useRef(createChartRefs());
  const latest = useRef(props);
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

  // Where each lower pane starts inside the chart, for its top-left label.
  const [paneLabels, setPaneLabels] = useState<{
    volume?: { top: number; left: number };
    oscillator?: { top: number; left: number };
  }>({});
  const hasReference = referencePrice !== undefined;
  const hasOscillator = oscillator !== undefined;
  const placePaneLabels = useCallback(() => {
    const element = container.current;
    const api = chart.current;
    if (!element || !api) return;
    const panes = api.panes();
    const first = panes[0]?.getHeight();
    if (first === undefined) return;
    const left =
      element.offsetLeft +
      (api.options().leftPriceScale.visible ? api.priceScale("left").width() : 0);
    const volume = showVolume ? { top: element.offsetTop + first + 1, left } : undefined;
    const second = showVolume ? panes[1]?.getHeight() : 0;
    const osc =
      hasOscillator && (!showVolume || second !== undefined)
        ? { top: element.offsetTop + first + (showVolume ? (second ?? 0) + 2 : 1), left }
        : undefined;
    setPaneLabels(prev =>
      prev.volume?.top === volume?.top &&
      prev.volume?.left === volume?.left &&
      prev.oscillator?.top === osc?.top &&
      prev.oscillator?.left === osc?.left
        ? prev
        : { volume, oscillator: osc },
    );
  }, [showVolume, hasOscillator]);
  useEffect(() => {
    if (!showVolume && !hasOscillator) return;
    const element = container.current!;
    // The engine resizes itself on the next frame.
    const observer = new ResizeObserver(() => requestAnimationFrame(placePaneLabels));
    observer.observe(element);
    return () => observer.disconnect();
  }, [showVolume, hasOscillator, placePaneLabels]);
  useEffect(() => {
    if (!showVolume && !hasOscillator) return;
    const frame = requestAnimationFrame(placePaneLabels);
    return () => cancelAnimationFrame(frame);
  }, [showVolume, hasOscillator, hasReference, series, placePaneLabels]);

  const [view, setView] = useState({ start: 0, end: series.points.length });
  const points = series.points;
  const count = points.length;
  const start = Math.min(view.start, Math.max(0, count - 1));
  const end = Math.max(start + 1, Math.min(view.end, count));
  const size = end - start;
  const selected =
    activeFocus === null ? -1 : points.findIndex(p => p.timestamp_ns === activeFocus);
  let lastObserved = end - 1;
  while (count && lastObserved > start && pointValue(points[lastObserved]) === null) lastObserved--;
  const index = Math.max(start, Math.min(end - 1, selected < 0 ? lastObserved : selected));

  const recordRange = useCallback((first: number, last: number, custom: boolean) => {
    const p = latest.current.series.points;
    refs.current.saved = {
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
    const state = refs.current;
    const css = getComputedStyle(element);
    const text = cssColor(css, "--muted", "#999faa");
    const grid = cssColor(css, "--grid", "#23262c");
    const separator = cssColor(css, "--line-strong", "#314054");
    const accent = cssColor(css, "--accent", "#38d6f2");
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
        panes: { separatorColor: separator, separatorHoverColor: withAlpha(accent, "44") },
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
      if (!range || state.updating) return;
      const p = latest.current.series.points;
      const first = Math.max(0, Math.round(range.from));
      const last = Math.min(p.length, Math.round(range.to) + 1);
      if (!p.length || last <= first) return;
      if (first === state.saved.start && last === state.saved.end) return;
      recordRange(first, last, true);
    };
    api.timeScale().subscribeVisibleLogicalRangeChange(rangeChanged);
    const resizeObserver =
      typeof ResizeObserver === "undefined"
        ? null
        : new ResizeObserver(() => {
            if (state.saved.custom) return;
            const currentSeries = latest.current.series;
            const total = currentSeries.points.length;
            if (!total || currentSeries.kind !== "candles") return;
            const width = defaultVisibleCount(currentSeries.kind, total, element.clientWidth);
            const first = Math.max(0, total - width);
            if (first === state.saved.start && total === state.saved.end) return;
            state.updating = true;
            recordRange(first, total, false);
            api.timeScale().setVisibleLogicalRange({ from: first, to: total - 1 });
            state.updating = false;
          });
    resizeObserver?.observe(element);
    api.subscribeCrosshairMove(event => {
      if (state.updating || !event.sourceEvent) return;
      const next =
        event.time !== undefined
          ? (latest.current.series.points[Number(event.time) - 1]?.timestamp_ns ?? null)
          : null;
      setFocus(next);
      latest.current.onFocusChange?.(next);
    });
    return () => {
      resizeObserver?.disconnect();
      disposeChartRefs(api, state);
      api.remove();
      chart.current = null;
    };
  }, [recordRange]);

  const referenceValue = referencePrice?.value;
  const referenceLabel = referencePrice?.label;
  useLayoutEffect(() => {
    const api = chart.current;
    const element = container.current;
    if (!api || !element) return;
    syncChartSeries(api, element, refs.current, {
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
      formatPrice: props.formatPrice,
      recordRange,
    });
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
    recordRange,
    quoteColors,
    domQuoteColors,
  ]);

  useLayoutEffect(() => {
    syncUserPriceLines(container.current, refs.current, priceLines);
  }, [priceLines, series, fillArea, baselineValue, quoteColors, domQuoteColors]);

  useLayoutEffect(() => {
    syncVerticalMarkers(chart.current, container.current, refs.current, verticalMarkers, count);
  }, [verticalMarkers, count, quoteColors, domQuoteColors]);

  useLayoutEffect(() => {
    syncTradeMarkersAndExtremes(
      chart.current,
      container.current,
      refs.current,
      series,
      trades,
      showPriceExtremes,
      start,
      end,
    );
  }, [series, showPriceExtremes, trades, start, end, quoteColors, domQuoteColors]);

  useLayoutEffect(() => {
    syncCrosshair(chart.current, refs.current, series, selected, fillArea, baselineValue);
  }, [selected, series, fillArea, baselineValue]);

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
            type="button"
            aria-label={controls.earlier}
            disabled={start === 0}
            onClick={() => pan(end - Math.max(1, Math.round(size / 5)))}
          >
            <Icon name="chevronLeft" size={14} />
          </button>
          <button
            type="button"
            aria-label={controls.zoomIn}
            disabled={size <= Math.min(5, count)}
            onClick={() => zoom(0.8)}
          >
            <Icon name="plus" size={14} />
          </button>
          <button
            type="button"
            aria-label={controls.zoomOut}
            disabled={size === count}
            onClick={() => zoom(1.25)}
          >
            <Icon name="minus" size={14} />
          </button>
          <button
            type="button"
            aria-label={controls.later}
            disabled={end === count}
            onClick={() => pan(end + Math.max(1, Math.round(size / 5)))}
          >
            <Icon name="chevronRight" size={14} />
          </button>
          <button type="button" onClick={last}>
            {controls.latest}
          </button>
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
        onPointerMove={event => {
          if (event.buttons && (showVolume || hasOscillator))
            requestAnimationFrame(placePaneLabels);
        }}
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
      {showVolume && paneLabels.volume && (
        <div
          className="price-chart-pane-label"
          data-chart-volume-axis
          style={{ top: paneLabels.volume.top, left: paneLabels.volume.left }}
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
      {oscillator && paneLabels.oscillator && (
        <div
          className="price-chart-pane-label"
          data-oscillator={oscillator.label}
          style={{ top: paneLabels.oscillator.top, left: paneLabels.oscillator.left }}
        >
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
