import { useEffect, useState, type ReactNode } from "react";
import { IntradayChart } from "@asterion/terminal/charts/IntradayChart";
import { PriceChart, type Candle } from "@asterion/terminal/charts/PriceChart";
import { timestamp } from "../../src/bridge/client";
import type { Minutes, QuoteRow } from "../../src/generated/asterion/history_pb";
import type { MarketView } from "../../src/host/market";
import {
  ErrorNotice,
  asDisplayError,
  getLocale,
  marketData,
  translate,
  useWorkspaceDraft,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { ContractHistory } from "./ContractHistory";
import { QuoteChart } from "./QuoteChart";
import { quoteChange, quoteStatus } from "./QuoteTable";
import { chartTime } from "./chart-time";
import { intradaySlots, sessionRanges } from "./intraday-slots";
import { averagePeriods, movingAverage } from "./indicators";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

type Row = QuoteRow;
export type PanePeriod =
  "分时" | "Tick" | "1分" | "5分" | "10分" | "日K" | "周K" | "月K" | "季K" | "年K" | "历史K线";
const primary: PanePeriod[] = ["分时", "Tick", "1分", "5分", "10分", "日K"];
// 历史K线 opens downloaded datasets with their own period and paging controls.
const more: PanePeriod[] = ["周K", "月K", "季K", "年K", "历史K线"];
const historical = {
  日K: "day",
  周K: "week",
  月K: "month",
  季K: "quarter",
  年K: "year",
} as const;
const averageColors = ["#cdd0d7", "#e5b45a", "#d555ae", "#52aa76", "#4aabe0"];

// Polls the market service's minute bars of one contract while mounted. When
// the quote stream starts again, the series is dropped and asked for afresh.
function useIntraday(market: MarketView, row: Row) {
  const [series, setSeries] = useState<Minutes | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const { exchange, symbol } = row.instrument!;
  const generation = market.generation;
  useEffect(() => {
    let current = true;
    // One read at a time, so replies are taken in the order they were asked for.
    let pending = false;
    setSeries(null);
    setError("");
    const load = () => {
      if (pending) return;
      pending = true;
      void marketData({ case: "getMinutes", value: { exchange, symbol } })
        .then(next => {
          if (!current) return;
          setSeries(next);
          setError("");
        })
        .catch(reason => {
          if (current) setError(asDisplayError(reason));
        })
        .finally(() => {
          pending = false;
        });
    };
    load();
    const timer = window.setInterval(() => {
      if (!document.hidden) load();
    }, 3000);
    return () => {
      current = false;
      window.clearInterval(timer);
    };
  }, [exchange, symbol, generation]);
  return { series, error };
}

function IntradayPanel({ market, row }: { market: MarketView; row: Row }) {
  const { series, error } = useIntraday(market, row);
  const { exchange, symbol } = row.instrument!;
  const product = market.board.catalog?.contracts.find(
    item => item.instrument?.exchange === exchange && item.instrument.symbol === symbol,
  )?.product;
  const reference = series?.previousSettlement?.text ?? row.quote?.previousSettlement?.text ?? null;
  if (error && !series)
    return (
      <div className="market-chart-empty" role="alert">
        <ErrorNotice error={error} namespace="asterion.terminal.futures-market" />
      </div>
    );
  if (!series) return <div className="market-chart-empty">{t("读取中")}</div>;
  if (!series.bars.length || reference === null)
    return (
      <div className="market-chart-empty" role="status">
        {t("本交易日尚无分时记录")}
      </div>
    );
  const { slots, ticks } = intradaySlots(series, product ? sessionRanges(exchange, product) : null);
  const since = new Date(Number(series.observedFromMs ?? 0n)).toLocaleTimeString(getLocale(), {
    timeZone: "Asia/Shanghai",
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
  });
  return (
    <>
      <IntradayChart
        slots={slots}
        volumeLabel={t("成交量")}
        averageLabel={t("均价")}
        referenceLabel={t("昨结价")}
        ticks={ticks}
        reference={reference}
        label={t("分时")}
        describe={index => {
          const slot = slots[index];
          return (
            <span className="intraday-legend">
              <span>{slot.label}</span>
              <span>
                {t("价格")} <b>{slot.price ?? "—"}</b>
              </span>
              <span className="intraday-average-label">
                {t("均价")} <b>{slot.average ?? "—"}</b>
              </span>
              <span>
                {t("成交量")} <b>{slot.volume ?? "—"}</b>
              </span>
            </span>
          );
        }}
      />
      <p className="intraday-note">
        {t("行情服务观测")} · {series.tradingDay} · {t("自")} {since}
        {series.interrupted && ` · ${t("序列曾中断")}`}
      </p>
    </>
  );
}

function MinuteCandles({
  market,
  row,
  minutes,
}: {
  market: MarketView;
  row: Row;
  minutes: number;
}) {
  const { series, error } = useIntraday(market, row);
  return <ObservedCandles series={series} error={error} minutes={minutes} row={row} />;
}
function ObservedCandles({
  series,
  error,
  minutes,
  row,
}: {
  series: Minutes | null;
  error: DisplayError;
  minutes: number;
  row: Row;
}) {
  if (error && !series)
    return (
      <div className="market-chart-empty" role="alert">
        <ErrorNotice error={error} namespace="asterion.terminal.futures-market" />
      </div>
    );
  if (!series) return <div className="market-chart-empty">{t("读取中")}</div>;
  const candles: Candle[] = [];
  let key = -1;
  for (const bar of series.bars) {
    const bucket = Math.floor(Number(bar.startMs) / (minutes * 60000));
    const last = candles[candles.length - 1];
    if (bucket !== key || !last) {
      key = bucket;
      candles.push({
        timestamp_ns: `${bucket * minutes * 60000}000000`,
        open: bar.open!.text,
        high: bar.high!.text,
        low: bar.low!.text,
        close: bar.close!.text,
        volume: String(bar.volume),
      });
    } else {
      if (Number(bar.high!.text) > Number(last.high)) last.high = bar.high!.text;
      if (Number(bar.low!.text) < Number(last.low)) last.low = bar.low!.text;
      last.close = bar.close!.text;
      last.volume = String(Number(last.volume) + Number(bar.volume));
    }
  }
  if (!candles.length)
    return (
      <div className="market-chart-empty" role="status">
        {t("本交易日尚无分时记录")}
      </div>
    );
  return (
    <PriceChart
      compact
      fitHeight
      showPriceExtremes
      mirrorPriceAxis
      key={`${row.instrument!.exchange}.${row.instrument!.symbol}:${minutes}:${series.tradingDay}`}
      series={{ kind: "candles", points: candles }}
      overlays={averagePeriods.slice(0, 4).map((period, index) => ({
        label: `MA${period}`,
        color: averageColors[index],
        values: movingAverage(candles, period),
      }))}
      formatIndicatorValue={value => value.toFixed(2)}
      label={t("分钟 K 线")}
      volumeLabel={t("成交量")}
      formatTime={timestamp}
      formatAxisTime={ns => chartTime(ns, false)}
      describe={index => (
        <>
          {chartTime(candles[index].timestamp_ns, false)} · O {candles[index].open} · H{" "}
          {candles[index].high} · L {candles[index].low} · C {candles[index].close}
        </>
      )}
    />
  );
}

export function QuotePane({
  context,
  market,
  row,
  name,
  pane,
  initial,
  actions,
}: {
  context: TerminalContext;
  market: MarketView;
  row: Row;
  name: string;
  pane: string;
  initial: PanePeriod;
  actions?: ReactNode;
}) {
  const [period, setPeriod] = useWorkspaceDraft<PanePeriod>(`${pane}.period`, initial);
  const [moreOpen, setMoreOpen] = useState(false);
  const change = quoteChange(row);
  const tone = !change
    ? undefined
    : change.amount > 0
      ? "up"
      : change.amount < 0
        ? "down"
        : undefined;
  const status = quoteStatus(market, row);
  const { exchange, symbol } = row.instrument!;
  const identity = `${exchange}.${symbol}`;
  return (
    <section className="quote-pane" aria-label={`${name} ${t(period)}`}>
      <header className="quote-pane-header">
        <strong title={`${name} · ${symbol}`}>{name}</strong>
        <b data-tone={tone}>{row.quote?.last?.text ?? "—"}</b>
        {change && (
          <>
            <span data-tone={tone}>
              {change.amount > 0 ? "+" : ""}
              {Number(change.amount.toFixed(8)).toLocaleString(getLocale(), {
                maximumFractionDigits: 8,
              })}
            </span>
            <span data-tone={tone}>
              {change.percent > 0 ? "+" : ""}
              {change.percent.toFixed(2)}%
            </span>
          </>
        )}
        {status !== t("实时") && <small className="quote-pane-status">{status}</small>}
        <span className="panel-spacer" />
        {actions}
        <small className="quote-pane-symbol">{symbol}</small>
      </header>
      <nav className="quote-pane-periods" aria-label={t("图表周期")}>
        {primary.map(value => (
          <button key={value} aria-pressed={period === value} onClick={() => setPeriod(value)}>
            {t(value)}
          </button>
        ))}
        <span className="quote-pane-more">
          <button
            aria-expanded={moreOpen}
            aria-pressed={more.includes(period)}
            onClick={() => setMoreOpen(value => !value)}
          >
            {more.includes(period) ? t(period) : t("更多")} ▾
          </button>
          {moreOpen && (
            <span className="quote-pane-menu" role="menu">
              {more.map(value => (
                <button
                  key={value}
                  role="menuitemradio"
                  aria-checked={period === value}
                  onClick={() => {
                    setPeriod(value);
                    setMoreOpen(false);
                  }}
                >
                  {t(value)}
                </button>
              ))}
            </span>
          )}
        </span>
      </nav>
      <div className="quote-pane-body">
        {period === "分时" ? (
          <IntradayPanel key={identity} market={market} row={row} />
        ) : period === "Tick" ? (
          <QuoteChart market={market} row={row} />
        ) : period === "1分" || period === "5分" || period === "10分" ? (
          <MinuteCandles
            key={identity}
            market={market}
            row={row}
            minutes={Number.parseInt(period, 10)}
          />
        ) : (
          <ContractHistory
            key={identity}
            venue={exchange}
            symbol={symbol}
            context={context}
            preferenceKey={period === "历史K线" ? `${pane}.history-full` : `${pane}.history`}
            fixedPeriod={period === "历史K线" ? undefined : historical[period]}
          />
        )}
      </div>
    </section>
  );
}
