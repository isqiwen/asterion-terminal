import {
  CandlestickSeries,
  ColorType,
  createChart,
  type IChartApi,
  type LogicalRange,
  type UTCTimestamp,
} from "lightweight-charts";
import { useEffect, useRef } from "react";
import type { Bar } from "../data/client";
import type { ChartViewport } from "./chartState";

export function Chart({
  bars,
  colors = "china",
  viewport,
  onViewport,
}: {
  bars: Bar[];
  viewport: ChartViewport | null;
  onViewport: (range: { from: number; to: number }) => void;
  colors?: "china" | "international";
}) {
  const ref = useRef<HTMLDivElement>(null);
  const api = useRef<IChartApi | null>(null);
  const latest = useRef({ viewport, onViewport });
  latest.current = { viewport, onViewport };
  useEffect(() => {
    if (!ref.current) return;
    const theme = getComputedStyle(document.documentElement);
    const color = (name: string) => theme.getPropertyValue(name).trim();
    const chart = createChart(ref.current, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: color("--panel") },
        textColor: color("--muted"),
        fontFamily: color("--font-terminal"),
        fontSize: 11,
      },
      grid: {
        vertLines: { color: color("--grid") },
        horzLines: { color: color("--grid") },
      },
      timeScale: { timeVisible: true, lockVisibleTimeRangeOnResize: true },
    });
    const series = chart.addSeries(CandlestickSeries, {
      upColor:
        colors === "china" ? color("--quote-red") : color("--quote-green"),
      downColor:
        colors === "china" ? color("--quote-green") : color("--quote-red"),
      borderVisible: false,
      wickUpColor:
        colors === "china" ? color("--quote-red") : color("--quote-green"),
      wickDownColor:
        colors === "china" ? color("--quote-green") : color("--quote-red"),
    });
    series.setData(
      bars
        .map((b) => ({
          time: Math.floor(Date.parse(b.event_time) / 1000) as UTCTimestamp,
          open: Number(b.open),
          high: Number(b.high),
          low: Number(b.low),
          close: Number(b.close),
        }))
        .sort((a, b) => Number(a.time) - Number(b.time)),
    );
    api.current = chart;
    const saved = latest.current.viewport;
    if (saved) chart.timeScale().setVisibleLogicalRange(saved);
    else chart.timeScale().fitContent();
    const displayRange = () => {
      const range = chart.timeScale().getVisibleLogicalRange();
      if (ref.current && range)
        ref.current.dataset.visibleRange = JSON.stringify(range);
    };
    displayRange();
    const change = (range: LogicalRange | null) => {
      displayRange();
      if (
        range &&
        Number.isFinite(range.from) &&
        Number.isFinite(range.to) &&
        range.from < range.to
      )
        latest.current.onViewport({
          from: Number(range.from),
          to: Number(range.to),
        });
    };
    chart.timeScale().subscribeVisibleLogicalRangeChange(change);
    return () => {
      chart.timeScale().unsubscribeVisibleLogicalRangeChange(change);
      api.current = null;
      chart.remove();
    };
  }, [bars, colors]);
  useEffect(() => {
    if (!api.current || !viewport) return;
    const current = api.current.timeScale().getVisibleLogicalRange();
    if (
      !current ||
      Math.abs(current.from - viewport.from) > 0.00001 ||
      Math.abs(current.to - viewport.to) > 0.00001
    )
      api.current.timeScale().setVisibleLogicalRange(viewport);
  }, [viewport]);
  return <div className="chart" ref={ref} />;
}
