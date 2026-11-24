import { useId, useMemo, useRef, type ReactNode } from "react";
import { IntradayChart } from "@asterion/terminal/charts/IntradayChart";
import { PriceChart, type Candle, type ChartPriceLine } from "@asterion/terminal/charts/PriceChart";
import { timestamp } from "../../src/bridge/client";
import { Side } from "../../src/generated/asterion/common_pb";
import type { Minutes, QuoteRow } from "../../src/generated/asterion/market_pb";
import { OrderState } from "../../src/generated/asterion/trading_pb";
import { useMarketMinutes, type MarketView } from "../../src/host/market";
import { useTradingAccounts, type TradingView } from "../../src/host/trading-accounts";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { getLocale, translate } from "../../src/i18n";
import type { DisplayError } from "../../src/i18n/errors";
import { ContractHistory } from "./ContractHistory";
import { QuoteChart } from "./QuoteChart";
import { quoteChange, quoteStatus } from "./QuoteTable";
import { chartTime } from "./chart-time";
import { intradaySlots, sessionRanges } from "./intraday-slots";
import { averagePeriods, movingAverage } from "./indicators";
import { priceTickOf } from "./price-tick";
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
const averageColors = [
  "var(--text-strong)",
  "var(--series-1)",
  "var(--series-2)",
  "var(--series-3)",
  "var(--series-4)",
];

export function accountPriceLines(
  views: Readonly<Record<string, TradingView>>,
  market: MarketView,
  row: Row,
): ChartPriceLine[] {
  const { exchange, symbol } = row.instrument!;
  const catalog = market.board.catalog?.contracts.find(
    item =>
      item.instrument?.exchange === exchange &&
      item.instrument.symbol.toUpperCase() === symbol.toUpperCase(),
  );
  const accountIds = Object.keys(views);
  const lines: ChartPriceLine[] = [];
  for (const [accountId, view] of Object.entries(views)) {
    const live = view.live;
    if (!live) continue;
    const name = view.instance || view.account?.id || accountId;
    const prefix = accountIds.length > 1 ? `${name} ` : "";
    const term =
      live.policy?.contracts.find(
        item => item.instrument?.exchange === exchange && item.instrument.symbol === symbol,
      ) ??
      live.policy?.contracts.find(
        item =>
          !item.deliveryMonth &&
          Boolean(item.product) &&
          item.instrument?.exchange === exchange &&
          item.instrument.symbol === item.product &&
          symbol.toLowerCase().startsWith(item.product.toLowerCase()) &&
          /^\d{3,4}$/.test(symbol.slice(item.product.length)),
      );
    const multiplier = Number(term?.multiplier?.text ?? catalog?.multiplier ?? 0);
    const tickText = term?.priceTick?.text ?? catalog?.priceTick?.text ?? "";
    const decimals = Math.max(2, (tickText.split(".")[1] ?? "").length);
    for (const p of live.positions) {
      if (p.instrument?.exchange !== exchange || p.instrument.symbol !== symbol) continue;
      const total = Number(p.todayLots?.text ?? 0) + Number(p.yesterdayLots?.text ?? 0);
      const cost = Number(p.cost?.text ?? 0);
      if (!total || !multiplier || !cost) continue;
      const average = cost / (total * multiplier);
      if (!Number.isFinite(average) || average <= 0) continue;
      lines.push({
        value: average.toFixed(decimals),
        label: `${prefix}${p.side === Side.BUY ? t("多") : t("空")} ${total}`,
        tone: p.side === Side.BUY ? "up" : "down",
        dashed: false,
      });
    }
    for (const o of live.workingOrders) {
      if (
        !o.id ||
        o.instrument?.exchange !== exchange ||
        o.instrument.symbol !== symbol ||
        ![OrderState.SUBMITTED, OrderState.ACCEPTED, OrderState.PARTIALLY_FILLED].includes(o.state)
      )
        continue;
      const remaining = Number(o.lots?.text ?? 0) - Number(o.filledLots?.text ?? 0);
      const price = o.price?.text;
      if (!price || remaining <= 0 || !Number.isFinite(Number(price))) continue;
      lines.push({
        value: price,
        label: `${prefix}${t("挂单")} ${o.side === Side.BUY ? t("买") : t("卖")} ${remaining}`,
        tone: o.side === Side.BUY ? "up" : "down",
        dashed: true,
      });
    }
  }
  return lines;
}

function samePriceLines(a: readonly ChartPriceLine[], b: readonly ChartPriceLine[]) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; ++i) {
    const x = a[i]!;
    const y = b[i]!;
    if (
      x.value !== y.value ||
      x.label !== y.label ||
      x.tone !== y.tone ||
      x.dashed !== y.dashed ||
      x.axisLabelVisible !== y.axisLabelVisible
    )
      return false;
  }
  return true;
}

function IntradayPanel({
  market,
  row,
  priceLines,
}: {
  market: MarketView;
  row: Row;
  priceLines: readonly ChartPriceLine[];
}) {
  const { series, error } = useMarketMinutes(market, row);
  const { exchange, symbol } = row.instrument!;
  const product = market.board.catalog?.contracts.find(
    item => item.instrument?.exchange === exchange && item.instrument.symbol === symbol,
  )?.product;
  const reference = series?.previousSettlement?.text ?? row.quote?.previousSettlement?.text ?? null;
  const intraday = useMemo(
    () =>
      series?.bars.length
        ? intradaySlots(series, product ? sessionRanges(exchange, product) : null)
        : null,
    [series, exchange, product],
  );
  if (error && !series)
    return (
      <div className="market-chart-empty" role="status">
        {t("报价序列暂不可用")}
      </div>
    );
  if (!series) return <div className="market-chart-empty">{t("读取中")}</div>;
  if (!intraday || reference === null)
    return (
      <div className="market-chart-empty" role="status">
        {t("本交易日尚无分时记录")}
      </div>
    );
  const { slots, ticks } = intraday;
  const since = new Date(Number(series.observedFromMs ?? 0n)).toLocaleTimeString("sv-SE", {
    timeZone: "Asia/Shanghai",
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
  });
  return (
    <>
      <IntradayChart
        priceTick={priceTickOf(market, exchange, symbol)}
        slots={slots}
        volumeLabel={t("成交量")}
        openInterestLabel={t("持仓量")}
        averageLabel={t("均价")}
        referenceLabel={t("昨结价")}
        priceLines={priceLines}
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
              {slot.openInterest && (
                <span>
                  {t("持仓量")} <b>{slot.openInterest}</b>
                </span>
              )}
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
  priceLines,
}: {
  market: MarketView;
  row: Row;
  minutes: number;
  priceLines: readonly ChartPriceLine[];
}) {
  const { series, error } = useMarketMinutes(market, row);
  const { exchange, symbol } = row.instrument!;
  return (
    <ObservedCandles
      series={series}
      error={error}
      minutes={minutes}
      row={row}
      priceTick={priceTickOf(market, exchange, symbol)}
      priceLines={priceLines}
    />
  );
}
function ObservedCandles({
  series,
  error,
  minutes,
  row,
  priceTick,
  priceLines,
}: {
  series: Minutes | null;
  error: DisplayError;
  minutes: number;
  row: Row;
  priceTick?: string;
  priceLines?: readonly ChartPriceLine[];
}) {
  const prepared = useMemo(() => {
    if (!series) return null;
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
          openInterest: bar.openInterest?.text,
        });
      } else {
        if (Number(bar.high!.text) > Number(last.high)) last.high = bar.high!.text;
        if (Number(bar.low!.text) < Number(last.low)) last.low = bar.low!.text;
        last.close = bar.close!.text;
        last.volume = String(Number(last.volume) + Number(bar.volume));
        if (bar.openInterest?.text) last.openInterest = bar.openInterest.text;
      }
    }
    return {
      candles,
      chartSeries: { kind: "candles" as const, points: candles },
      overlays: averagePeriods.slice(0, 4).map((period, index) => ({
        label: `MA${period}`,
        color: averageColors[index],
        values: movingAverage(candles, period),
      })),
    };
  }, [series, minutes]);
  if (error && !series)
    return (
      <div className="market-chart-empty" role="status">
        {t("报价序列暂不可用")}
      </div>
    );
  if (!series || !prepared) return <div className="market-chart-empty">{t("读取中")}</div>;
  const { candles, chartSeries, overlays } = prepared;
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
      priceTick={priceTick}
      priceLines={priceLines}
      key={`${row.instrument!.exchange}.${row.instrument!.symbol}:${minutes}:${series.tradingDay}`}
      series={chartSeries}
      overlays={overlays}
      formatIndicatorValue={value => value.toFixed(2)}
      label={t("分钟 K 线")}
      volumeLabel={t("成交量")}
      openInterestLabel={t("持仓量")}
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
  const menuId = useId();
  const anchor = `--period-${menuId.replace(/:/g, "")}`;
  const moreRef = useRef<HTMLSpanElement>(null);
  const trigger = useRef<HTMLButtonElement>(null);
  const change = quoteChange(row);
  const tone = !change
    ? undefined
    : change.amount > 0
      ? "up"
      : change.amount < 0
        ? "down"
        : undefined;
  const status = quoteStatus(market, row);
  const { views: tradingViews } = useTradingAccounts();
  const nextPriceLines = accountPriceLines(tradingViews, market, row);
  const stablePriceLines = useRef<readonly ChartPriceLine[]>(nextPriceLines);
  if (!samePriceLines(stablePriceLines.current, nextPriceLines))
    stablePriceLines.current = nextPriceLines;
  const priceLines = stablePriceLines.current;
  const { exchange, symbol } = row.instrument!;
  const identity = `${exchange}.${symbol}`;
  return (
    <section className="quote-pane" aria-label={`${name} ${t(period)}`}>
      <header className="quote-pane-header">
        <div className="quote-pane-summary">
          <strong title={`${name} · ${symbol}`}>{name}</strong>
          {name !== symbol && <small className="quote-pane-symbol">{symbol}</small>}
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
        </div>
        {actions && <div className="quote-pane-actions">{actions}</div>}
      </header>
      <nav className="quote-pane-periods tabs" aria-label={t("图表周期")}>
        {primary.map(value => (
          <button
            key={value}
            type="button"
            aria-pressed={period === value}
            onClick={() => {
              setPeriod(value);
              moreRef.current?.hidePopover();
            }}
          >
            {t(value)}
          </button>
        ))}
        <span className="quote-pane-more">
          <button
            ref={trigger}
            type="button"
            style={{ anchorName: anchor }}
            {...{ commandfor: menuId, command: "toggle-popover" }}
            aria-pressed={more.includes(period)}
          >
            {more.includes(period) ? t(period) : t("更多")} ▾
          </button>
          <span
            key={identity}
            ref={moreRef}
            id={menuId}
            popover="auto"
            className="quote-pane-menu"
            style={{ positionAnchor: anchor }}
            role="group"
            aria-label={t("图表周期")}
          >
            {more.map(value => (
              <button
                key={value}
                type="button"
                aria-pressed={period === value}
                {...{ commandfor: menuId, command: "hide-popover" }}
                onClick={() => {
                  setPeriod(value);
                  trigger.current?.focus();
                }}
              >
                {t(value)}
              </button>
            ))}
          </span>
        </span>
      </nav>
      <div className="quote-pane-body">
        {period === "分时" ? (
          <IntradayPanel key={identity} market={market} row={row} priceLines={priceLines} />
        ) : period === "Tick" ? (
          <QuoteChart market={market} row={row} priceLines={priceLines} />
        ) : period === "1分" || period === "5分" || period === "10分" ? (
          <MinuteCandles
            key={identity}
            market={market}
            row={row}
            minutes={Number.parseInt(period, 10)}
            priceLines={priceLines}
          />
        ) : (
          <ContractHistory
            key={identity}
            venue={exchange}
            symbol={symbol}
            context={context}
            preferenceKey={period === "历史K线" ? `${pane}.history-full` : `${pane}.history`}
            fixedPeriod={period === "历史K线" ? undefined : historical[period]}
            priceLines={priceLines}
          />
        )}
      </div>
    </section>
  );
}
