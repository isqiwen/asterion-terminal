import { useLayoutEffect, useMemo, useRef, useState } from "react";
import {
  AreaSeries,
  ColorType,
  CrosshairMode,
  createYieldCurveChart,
  type ISeriesApi,
  type IYieldCurveChartApi,
} from "lightweight-charts";
import "./price-chart.css";

export type TermStructurePoint = {
  key: string;
  symbol: string;
  price: string;
};

export function deliveryMonthIndex(
  symbol: string,
  referenceYear = new Date().getUTCFullYear(),
): number | null {
  const match = /(\d{3,4})$/.exec(symbol.trim());
  if (!match) return null;
  const digits = match[1];
  let year: number;
  let month: number;
  if (digits.length === 4) {
    year = 2000 + Number(digits.slice(0, 2));
    month = Number(digits.slice(2));
  } else {
    const decade = Math.floor(referenceYear / 10) * 10;
    const digit = Number(digits[0]);
    year = decade + digit < referenceYear - 1 ? decade + 10 + digit : decade + digit;
    month = Number(digits.slice(1));
  }
  if (!Number.isInteger(month) || month < 1 || month > 12) return null;
  return year * 12 + (month - 1);
}

const monthLabel = (monthIndex: number) => {
  const year = Math.floor(monthIndex / 12) % 100;
  const month = (monthIndex % 12) + 1;
  return `${String(year).padStart(2, "0")}${String(month).padStart(2, "0")}`;
};

const withAlpha = (hex: string, alpha: string) =>
  /^#[0-9a-f]{6}$/i.test(hex) ? `${hex}${alpha}` : hex;

export function TermStructureChart({
  points,
  selectedKey,
  label,
  priceTick,
  referenceYear,
  onSelect,
}: {
  points: readonly TermStructurePoint[];
  selectedKey?: string;
  label: string;
  priceTick?: string;
  referenceYear?: number;
  onSelect?: (key: string) => void;
}) {
  const container = useRef<HTMLDivElement>(null);
  const chart = useRef<IYieldCurveChartApi | null>(null);
  const seriesRef = useRef<ISeriesApi<"Area", number> | null>(null);
  const latest = useRef({ onSelect });
  const [hoveredOffset, setHoveredOffset] = useState<number | null>(null);

  const curve = useMemo(() => {
    const byMonth = new Map<number, TermStructurePoint & { numeric: number; monthIndex: number }>();
    for (const point of points) {
      const numeric = Number(point.price);
      const monthIndex = deliveryMonthIndex(point.symbol, referenceYear);
      if (!point.price || !Number.isFinite(numeric) || numeric <= 0 || monthIndex === null)
        continue;
      byMonth.set(monthIndex, { ...point, numeric, monthIndex });
    }
    const sorted = [...byMonth.values()].sort((a, b) => a.monthIndex - b.monthIndex);
    if (sorted.length < 2) return null;
    const firstMonth = sorted[0].monthIndex;
    const items = sorted.map(item => ({ ...item, offset: item.monthIndex - firstMonth }));
    const byOffset = new Map(items.map(item => [item.offset, item]));
    return {
      firstMonth,
      span: Math.max(1, items[items.length - 1].offset),
      items,
      byOffset,
    };
  }, [points, referenceYear]);

  const latestCurve = useRef(curve);
  const hasCurve = curve !== null;
  const span = curve?.span ?? 1;
  useLayoutEffect(() => {
    latest.current = { onSelect };
    latestCurve.current = curve;
  });

  useLayoutEffect(() => {
    if (!container.current || !hasCurve) return;
    const css = getComputedStyle(container.current);
    const color = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const text = color("--muted", "#999faa");
    const grid = color("--grid", "#23262c");
    const accent = color("--accent", "#38d6f2");
    const api = createYieldCurveChart(container.current, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: "transparent" },
        textColor: text,
        fontSize: 11,
        attributionLogo: false,
      },
      grid: { vertLines: { color: grid }, horzLines: { color: grid } },
      crosshair: { mode: CrosshairMode.Magnet },
      rightPriceScale: {
        visible: true,
        borderVisible: false,
        scaleMargins: { top: 0.14, bottom: 0.14 },
      },
      leftPriceScale: { visible: false, borderVisible: false },
      timeScale: {
        borderVisible: false,
        fixLeftEdge: true,
        fixRightEdge: true,
      },
      handleScroll: false,
      handleScale: false,
      yieldCurve: {
        baseResolution: 1,
        startTimeRange: 0,
        minimumTimeRange: span,
        formatTime: (months: number) => {
          const current = latestCurve.current;
          if (!current) return "";
          return (
            current.byOffset.get(months)?.symbol ??
            monthLabel(current.firstMonth + Math.round(months))
          );
        },
      },
    });
    const area = api.addSeries(AreaSeries, {
      lineColor: accent,
      topColor: withAlpha(accent, "2e"),
      bottomColor: "transparent",
      lineWidth: 2,
      pointMarkersVisible: true,
      pointMarkersRadius: 3,
      lastValueVisible: false,
      priceLineVisible: false,
    });
    chart.current = api;
    seriesRef.current = area;
    api.subscribeCrosshairMove(event => {
      if (!event.sourceEvent) return;
      setHoveredOffset(event.time !== undefined ? Number(event.time) : null);
    });
    api.subscribeClick(event => {
      if (event.time === undefined) return;
      const hit = latestCurve.current?.byOffset.get(Number(event.time));
      if (hit) latest.current.onSelect?.(hit.key);
    });
    return () => {
      seriesRef.current = null;
      chart.current = null;
      api.remove();
    };
  }, [hasCurve, span]);

  useLayoutEffect(() => {
    const api = chart.current;
    const area = seriesRef.current;
    if (!api || !area || !curve) return;
    const tick = priceTick ? Number(priceTick) : null;
    const decimals = priceTick ? (priceTick.split(".")[1]?.length ?? 0) : 2;
    area.applyOptions({
      priceFormat: {
        type: "custom",
        minMove: tick && tick > 0 ? tick : 0.01,
        formatter: (value: number) => value.toFixed(decimals),
      },
    });
    area.setData(curve.items.map(item => ({ time: item.offset, value: item.numeric })));
    api.timeScale().fitContent();
  }, [curve, priceTick]);

  if (!curve) return null;
  const active =
    (hoveredOffset !== null ? curve.byOffset.get(hoveredOffset) : undefined) ??
    curve.items.find(item => item.key === selectedKey) ??
    curve.items[0];
  const spread = curve.items[curve.items.length - 1].numeric - curve.items[0].numeric;
  const spreadDecimals = priceTick ? (priceTick.split(".")[1]?.length ?? 0) : 2;

  return (
    <div className="term-structure-chart">
      <p className="term-structure-cursor">
        <span>{label}</span> · <b>{active.symbol}</b> <b>{active.price}</b> ·{" "}
        <span>
          {curve.items[curve.items.length - 1].symbol}−{curve.items[0].symbol}{" "}
          {spread > 0 ? "+" : ""}
          {spread.toFixed(spreadDecimals)}
        </span>
      </p>
      <div
        className="term-structure-canvas"
        ref={container}
        role="img"
        aria-label={label}
        onPointerLeave={() => setHoveredOffset(null)}
      />
    </div>
  );
}
