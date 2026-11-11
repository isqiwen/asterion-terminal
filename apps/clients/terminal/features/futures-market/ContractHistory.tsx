import { dailyChartDate } from "../../src/ui/charts/daily-chart";
import { useMarket } from "../../src/host/market";
import { chartTime } from "./chart-time";
import { priceTickOf } from "./price-tick";
import { averagePeriods, movingAverage } from "./indicators";
import { useEffect, useState } from "react";
import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import { timestamp } from "../../src/bridge/client";
import { lab } from "../../src/bridge/lab";
import type { ChartBars, StoredContract } from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { translate } from "../../src/i18n";
import { type DisplayError, ErrorNotice, asDisplayError } from "../../src/i18n/errors";
import { type ChartPeriod, protocolPeriod } from "../../src/ui/periods";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

type Period = ChartPeriod;
const minutePeriods = [1, 5, 10, 15, 30, 60];
const calendarPeriods = [
  ["day", "日K"],
  ["week", "周线"],
  ["month", "月线"],
  ["quarter", "季线"],
  ["year", "年线"],
] as const;
const page = 100;

// A contract's stored bars from the research dataset: minutes gathered into
// longer periods, or days into calendar periods, a hundred at a time.
export function ContractHistory({
  venue,
  symbol,
  historyContractId,
  datasetVersion = "",
  context,
  preferLongest = false,
  preferenceKey = preferLongest ? "history-long" : "history-main",
  fixedPeriod,
}: {
  venue: string;
  symbol: string;
  historyContractId?: string;
  // A fixed research dataset version; empty for the current one.
  datasetVersion?: string;
  context: TerminalContext;
  preferLongest?: boolean;
  preferenceKey?: string;
  // Chosen by an enclosing period tab; hides this chart's own period controls.
  fixedPeriod?: Period;
}) {
  const { snapshot, busy, navigate } = context;
  const market = useMarket();
  const contractId =
    historyContractId ??
    market?.board.catalog?.contracts.find(
      item =>
        item.instrument?.exchange === venue &&
        item.instrument.symbol.toUpperCase() === symbol.toUpperCase(),
    )?.contract;
  const [retry, setRetry] = useState(0);
  // What the research dataset holds of this contract.
  const [stored, setStored] = useState<{
    key: string;
    contract?: StoredContract;
    error: DisplayError;
  }>();
  const endpoint = snapshot?.warehouse_endpoint;
  const address =
    endpoint && "host" in endpoint
      ? `${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`
      : "";
  const storedKey = JSON.stringify([address, contractId, datasetVersion, retry]);
  useEffect(() => {
    if (!contractId) return;
    let active = true;
    void lab({ case: "listStoredContracts", value: { contract: contractId, datasetVersion } })
      .then(
        reply => active && setStored({ key: storedKey, contract: reply.contracts[0], error: "" }),
      )
      .catch(reason => active && setStored({ key: storedKey, error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [contractId, datasetVersion, storedKey]);
  const known = stored?.key === storedKey ? stored : undefined;
  const hasDays = !!known?.contract?.daily?.days;
  const hasMinutes = !!known?.contract?.minutes?.days;
  const [period, setPeriod] = useWorkspaceDraft<Period | null>(`${preferenceKey}.period`, null);
  const initialPeriod = !known
    ? undefined
    : preferLongest
      ? hasDays
        ? "day"
        : hasMinutes
          ? 60
          : undefined
      : hasMinutes
        ? 1
        : hasDays
          ? "day"
          : undefined;
  const activePeriod = fixedPeriod ?? period ?? initialPeriod;
  const daily = typeof activePeriod === "string";
  const available = daily ? hasDays : hasMinutes;
  const [showMacd, setShowMacd] = useWorkspaceDraft(`${preferenceKey}.macd`, true);
  const [showAverages, setShowAverages] = useWorkspaceDraft(`${preferenceKey}.averages`, true);
  const barsKey = JSON.stringify([address, contractId, datasetVersion, activePeriod, retry]);
  const [drawn, setDrawn] = useState<{ key: string; bars?: ChartBars; error: DisplayError }>();
  useEffect(() => {
    if (!contractId || activePeriod === undefined || !available) return;
    let active = true;
    void lab({
      case: "getChartBars",
      value: { contract: contractId, period: protocolPeriod(activePeriod), datasetVersion },
    })
      .then(bars => active && setDrawn({ key: barsKey, bars, error: "" }))
      .catch(reason => active && setDrawn({ key: barsKey, error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [barsKey, contractId, datasetVersion, activePeriod, available]);
  const current = drawn?.key === barsKey ? drawn : undefined;
  const all = current?.bars?.bars ?? [];
  // The latest page first; paging keeps its place per contract and period.
  const [paging, setPaging] = useWorkspaceDraft(`${preferenceKey}.paging`, { key: "", end: 0 });
  const end = paging.key === barsKey ? Math.min(paging.end, all.length) : all.length;
  const begin = Math.max(0, end - page);
  const setEnd = (value: number) => setPaging({ key: barsKey, end: value });
  const shown = all.slice(begin, end);
  const candles = shown.map(bar => ({
    timestamp_ns: String(bar.endMs * 1_000_000n),
    open: bar.open?.text ?? "",
    high: bar.high?.text ?? "",
    low: bar.low?.text ?? "",
    close: bar.close?.text ?? "",
    volume: bar.volume?.text ?? "",
  }));
  // Days are labelled 15:00 Beijing time, the same date in UTC.
  const formatTime = (ns: string) => (daily ? dailyChartDate(ns) : timestamp(ns));
  const colors = ["#cdd0d7", "#e5b45a", "#d555ae", "#52aa76", "#4aabe0"];
  const overlays = showAverages
    ? averagePeriods.map((value, index) => ({
        label: `MA${value}`,
        color: colors[index],
        values: movingAverage(
          all.map(bar => ({ close: bar.close?.text ?? "" })),
          value,
        ).slice(begin, end),
      }))
    : [];
  const error = known?.error || current?.error;
  const loading = !!contractId && (!known || (available && !current));
  const disabled = busy || loading;
  return (
    <section className="market-history-chart" aria-label={t("合约历史 K 线")} aria-busy={loading}>
      {fixedPeriod === undefined && (
        <header className="market-chart-title">
          <strong>{symbol}</strong>
          <span>{t("历史 K 线")}</span>
        </header>
      )}
      {error && (
        <div role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.futures-market" />
          <button disabled={busy} onClick={() => setRetry(value => value + 1)}>
            {t("重试")}
          </button>
        </div>
      )}
      <div className="market-chart-toolbar">
        <div
          className="market-periods"
          role="group"
          aria-label={t("历史周期")}
          hidden={fixedPeriod !== undefined}
        >
          {minutePeriods.map(value => (
            <button
              key={value}
              aria-label={`${value} min`}
              title={hasMinutes ? undefined : t("尚无该周期的数据")}
              aria-pressed={activePeriod === value}
              disabled={disabled || !hasMinutes}
              onClick={() => setPeriod(value)}
            >
              {value}
              {t("分")}
            </button>
          ))}
          {calendarPeriods.map(([value, label]) => (
            <button
              key={value}
              aria-label={t(label)}
              aria-pressed={activePeriod === value}
              disabled={disabled || !hasDays}
              title={hasDays ? undefined : t("尚无该周期的数据")}
              onClick={() => setPeriod(value)}
            >
              {t(label)}
            </button>
          ))}
        </div>
        <button
          aria-label={t("显示均线")}
          aria-pressed={showAverages}
          onClick={() => setShowAverages(value => !value)}
        >
          MA
        </button>
        <button
          aria-label={t("显示成交量")}
          aria-pressed={!showMacd}
          onClick={() => setShowMacd(false)}
        >
          {t("成交量")}
        </button>
        <button
          aria-label={t("显示 MACD")}
          aria-pressed={showMacd}
          onClick={() => setShowMacd(value => !value)}
        >
          MACD
        </button>
      </div>
      {shown.length ? (
        <>
          <PriceChart
            compact
            fitHeight
            showPriceExtremes
            mirrorPriceAxis
            priceTick={priceTickOf(market, venue, symbol)}
            formatIndicatorValue={value => value.toFixed(2)}
            showVolume={!showMacd}
            overlays={overlays}
            oscillator={
              showMacd
                ? {
                    label: "MACD(12,26,9)",
                    emptyLabel: t("当前区间无 MACD 数据"),
                    histogramLabel: "MACD",
                    lines: [
                      {
                        label: "DIFF",
                        color: "#cdd0d7",
                        values: shown.map(bar => bar.macdDiff ?? null),
                      },
                      {
                        label: "DEA",
                        color: "#e5b45a",
                        values: shown.map(bar => bar.macdSignal ?? null),
                      },
                    ],
                    histogram: shown.map(bar => bar.macdHistogram ?? null),
                  }
                : undefined
            }
            key={`${barsKey}:${begin}`}
            series={{ kind: "candles", points: candles }}
            label={t("合约历史 K 线")}
            volumeLabel={t("成交量")}
            formatTime={formatTime}
            formatAxisTime={daily ? formatTime : ns => chartTime(ns, true)}
            describe={index => (
              <>
                {formatTime(candles[index].timestamp_ns)} · O {candles[index].open} · H{" "}
                {candles[index].high} · L {candles[index].low} · C {candles[index].close}
              </>
            )}
          />
          <div className="market-chart-toolbar market-history-pages">
            <span>
              {begin + 1}–{end} / {all.length}
            </span>
            <button
              disabled={disabled || !begin}
              onClick={() => setEnd(Math.min(page, all.length))}
            >
              {t("最早数据")}
            </button>
            <button
              disabled={disabled || !begin}
              onClick={() => setEnd(Math.max(page, end - page))}
            >
              {t("向前")}
            </button>
            <button
              disabled={disabled || end >= all.length}
              onClick={() => setEnd(Math.min(all.length, end + page))}
            >
              {t("向后")}
            </button>
            <button disabled={disabled || end >= all.length} onClick={() => setEnd(all.length)}>
              {t("最新数据")}
            </button>
          </div>
          <div className="market-chart-source">{t("研究数据 · 历史数据 · 非实时")}</div>
        </>
      ) : (
        <div className="market-chart-empty" role="status">
          <p>
            {t(
              loading
                ? "读取中"
                : !contractId
                  ? "当前目录没有这个合约的月份"
                  : available || current
                    ? "暂无历史记录"
                    : "该月份合约尚无已采集历史数据",
            )}
          </p>
          {!loading && contractId && !available && (
            <button onClick={() => navigate("workspace.coverage")}>{t("采集历史数据")}</button>
          )}
        </div>
      )}
    </section>
  );
}
