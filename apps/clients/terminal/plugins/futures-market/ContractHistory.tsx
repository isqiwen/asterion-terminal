import { dailyChartBar, dailyChartDate } from "../../src/ui/charts/daily-chart";
import { chartTime } from "./chart-time";
import { averagePeriods, historyLookback, movingAverage } from "./indicators";
import { useEffect, useRef, useState } from "react";
import { PriceChart } from "@asterion/client-ui/PriceChart";
import {
  timestamp,
  type HistoryPage,
  type DailyPage,
  type ResearchTask,
} from "../../src/bridge/client";
import {
  translate,
  useWorkspaceDraft,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const taskPeriod = (task: ResearchTask): number | "day" | undefined =>
  task.kind === "daily_download" ? "day" : task.minute_interval_minutes;
const t = (key: string) => translate("asterion.terminal.futures-market", key);

export function ContractHistory({
  venue,
  symbol,
  context,
  preferLongest = false,
  preferenceKey = preferLongest ? "history-long" : "history-main",
  fixedPeriod,
}: {
  venue: string;
  symbol: string;
  context: TerminalContext;
  preferLongest?: boolean;
  preferenceKey?: string;
  // Chosen by an enclosing period tab; hides this chart's own period controls.
  fixedPeriod?: number | "day" | "week" | "month" | "quarter" | "year";
}) {
  const { snapshot, query, busy, navigate } = context;
  const datasets = (snapshot?.research?.tasks ?? [])
    .filter(
      task =>
        (task.kind === "minute_download" || task.kind === "daily_download") &&
        task.state === "succeeded" &&
        task.instrument.toUpperCase() === `${venue}/${symbol}`.toUpperCase(),
    )
    .sort((a, b) => b.submission_sequence - a.submission_sequence);
  const periods = [
    ...new Set(
      datasets.flatMap(task =>
        task.kind === "minute_download" && task.minute_interval_minutes
          ? [task.minute_interval_minutes]
          : [],
      ),
    ),
  ].sort((a, b) => a - b);
  const connection = snapshot?.research?.connection_id;
  const [selection, setSelection] = useState<{ connection: string | undefined; id: string }>({
    connection,
    id: "",
  });
  const selected = selection.connection === connection ? selection.id : "";
  const setSelected = (id: string) => setSelection({ connection, id });
  // The chart pane keeps its display choices when a different contract is selected.
  // Dataset IDs and paging remain contract-local; unavailable periods never fall back.
  const [period, setPeriod] = useWorkspaceDraft<
    number | "day" | "week" | "month" | "quarter" | "year" | null
  >(`${preferenceKey}.period`, null);
  const initialDataset =
    (preferLongest
      ? (datasets.find(task => task.kind === "daily_download") ??
        datasets.find(task => task.minute_interval_minutes === periods.at(-1)))
      : undefined) ?? datasets[0];
  const activePeriod =
    fixedPeriod ?? period ?? (initialDataset ? taskPeriod(initialDataset) : undefined);
  useEffect(() => {
    // A tab-imposed period is not this chart's own remembered choice.
    if (fixedPeriod === undefined && period === null && activePeriod !== undefined)
      setPeriod(activePeriod);
  }, [fixedPeriod, period, activePeriod, setPeriod]);
  const daily = typeof activePeriod === "string";
  const sourcePeriod = daily ? "day" : activePeriod;
  const id =
    datasets.find(task => task.id === selected && taskPeriod(task) === sourcePeriod)?.id ??
    datasets.find(task => taskPeriod(task) === sourcePeriod)?.id ??
    "";
  const datasetKey = JSON.stringify([connection, id, activePeriod]);
  const [paging, setPaging] = useState({ key: "", offset: 0 });
  const offset = paging.key === datasetKey ? paging.offset : 0;
  useEffect(() => {
    setPaging({ key: datasetKey, offset: 0 });
  }, [datasetKey]);
  const setOffset = (offset: number) => setPaging({ key: datasetKey, offset });
  const [showMacd, setShowMacd] = useWorkspaceDraft(`${preferenceKey}.macd`, true);
  const [showAverages, setShowAverages] = useWorkspaceDraft(`${preferenceKey}.averages`, true);
  const [retry, setRetry] = useState(0);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [response, setResponse] = useState<{ key: string; page: HistoryPage | DailyPage } | null>(
    null,
  );
  const invoke = useRef(query);
  useEffect(() => {
    invoke.current = query;
  }, [query]);
  const online = !!snapshot?.research?.online;
  useEffect(() => {
    let current = true;
    setError("");
    if (!id || !online) {
      setLoading(false);
      return;
    }
    setLoading(true);
    void invoke
      .current(daily ? "research.daily.page" : "research.minutes.page", {
        ...(daily ? { period: activePeriod } : {}),
        include_macd: true,
        id,
        offset: Math.max(0, offset - historyLookback),
        limit: 100 + historyLookback,
        start: "",
        end: "",
      })
      .then(result => {
        if (current) {
          const page = daily ? result.daily_page : result.history_page;
          if (
            !page ||
            page.id !== id ||
            page.source !== (daily ? "tushare.fut_daily" : "tushare.ft_mins") ||
            (daily && (!("period" in page) || page.period !== activePeriod))
          )
            throw new Error(t("返回的数据与所选数据集不匹配"));
          setResponse({ key: datasetKey, page });
        }
      })
      .catch(reason => {
        if (current) setError(asDisplayError(reason));
      })
      .finally(() => {
        if (current) setLoading(false);
      });
    return () => {
      current = false;
    };
  }, [datasetKey, id, offset, online, retry, daily, activePeriod]);
  const responsePage = response?.key === datasetKey ? response.page : null;
  const rawPage =
    responsePage?.id === id &&
    responsePage.ts_code.split(".")[0].toUpperCase() === symbol.toUpperCase()
      ? responsePage
      : null;
  const leading = rawPage?.offset ? historyLookback : 0;
  const page = rawPage
    ? {
        ...rawPage,
        offset: rawPage.offset + leading,
        bars: rawPage.bars.slice(leading, leading + 100),
      }
    : null;
  const chartBars = page?.bars.map(bar => ("trading_day" in bar ? dailyChartBar(bar) : bar)) ?? [];
  const formatTime = daily ? dailyChartDate : timestamp;
  const colors = ["#cdd0d7", "#e5b45a", "#d555ae", "#52aa76", "#4aabe0"];
  const overlays =
    showAverages && rawPage
      ? averagePeriods.map((period, index) => ({
          label: `MA${period}`,
          color: colors[index],
          values: movingAverage(rawPage.bars, period).slice(leading, leading + 100),
        }))
      : [];
  const disabled = busy || loading || !online;
  return (
    <section className="market-history-chart" aria-label={t("合约历史 K 线")} aria-busy={loading}>
      {fixedPeriod === undefined && (
        <header className="market-chart-title">
          <strong>{symbol}</strong>
          <span>{t("历史 K 线")}</span>
        </header>
      )}
      {!online && !page ? (
        <div className="market-chart-empty" role="status">
          <p>{t("历史数据服务未连接")}</p>
          <button onClick={() => navigate("workspace.data", { page: "history" })}>
            {t("管理历史数据")}
          </button>
        </div>
      ) : datasets.length || period !== null || fixedPeriod !== undefined ? (
        <>
          <div className="market-chart-toolbar">
            <div
              className="market-periods"
              role="group"
              aria-label={t("历史周期")}
              hidden={fixedPeriod !== undefined}
            >
              {[
                ...new Set([
                  1,
                  5,
                  10,
                  ...periods,
                  ...(typeof activePeriod === "number" ? [activePeriod] : []),
                ]),
              ]
                .sort((a, b) => a - b)
                .map(period => (
                  <button
                    key={period}
                    aria-label={`${period} min`}
                    title={periods.includes(period) ? undefined : t("尚无该周期的数据")}
                    aria-pressed={activePeriod === period}
                    disabled={disabled || !periods.includes(period)}
                    onClick={() => {
                      setPeriod(period);
                      setSelected(
                        datasets.find(task => task.minute_interval_minutes === period)!.id,
                      );
                      setOffset(0);
                    }}
                  >
                    {period}
                    {t("分")}
                  </button>
                ))}
              {(
                [
                  ["day", "日K"],
                  ["week", "周线"],
                  ["month", "月线"],
                  ["quarter", "季线"],
                  ["year", "年线"],
                ] as const
              ).map(([value, label]) => (
                <button
                  key={value}
                  aria-label={t(label)}
                  aria-pressed={activePeriod === value}
                  disabled={disabled || !datasets.some(task => task.kind === "daily_download")}
                  title={
                    datasets.some(task => task.kind === "daily_download")
                      ? undefined
                      : t("尚无该周期的数据")
                  }
                  onClick={() => {
                    setPeriod(value);
                    setSelected(datasets.find(task => task.kind === "daily_download")!.id);
                    setOffset(0);
                  }}
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
            {id && (
              <details className="market-dataset-choice">
                <summary>{t("数据版本")}</summary>
                <select
                  aria-label={t("历史数据版本")}
                  value={id}
                  onChange={event => {
                    setSelected(event.target.value);
                    const chosen = datasets.find(task => task.id === event.target.value);
                    setPeriod(
                      chosen
                        ? chosen.kind === "daily_download" && daily
                          ? activePeriod
                          : (taskPeriod(chosen) ?? null)
                        : null,
                    );
                    setOffset(0);
                  }}
                  disabled={disabled}
                >
                  {datasets.map(task => (
                    <option key={task.id} value={task.id}>
                      {task.source_name} · #{task.submission_sequence}
                    </option>
                  ))}
                </select>
                <p className="subtle">
                  {t("MACD 从数据集首根收盘价起算，前 33 根为预热；图形指标不用于交易执行。")}
                </p>
                {daily && activePeriod !== "day" && (
                  <p className="subtle">
                    {t(
                      "周线按周一至周日，月线、季线和年线按自然月、自然季度和自然年聚合已有日线；日期为最后一条来源日期，缺失日期不补齐，末期可能尚未结束。",
                    )}
                  </p>
                )}
              </details>
            )}
          </div>
          {error && (
            <div role="alert">
              <ErrorNotice error={error} namespace="asterion.terminal.futures-market" />
              <button disabled={disabled} onClick={() => setRetry(value => value + 1)}>
                {t("重试")}
              </button>
            </div>
          )}
          {!online && <p className="subtle">{t("历史数据服务未连接")}</p>}
          {!id ? (
            <div className="market-chart-empty" role="status">
              <p>
                {t(datasets.length ? "该合约尚无所选周期的数据" : "该月份合约尚无已下载历史数据")}
              </p>
              <button onClick={() => navigate("workspace.data", { page: "history" })}>
                {t("下载历史数据")}
              </button>
            </div>
          ) : page?.bars.length ? (
            <>
              <PriceChart
                compact
                fitHeight
                showPriceExtremes
                mirrorPriceAxis
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
                            values: page.bars.map(bar => bar.macd?.diff ?? null),
                          },
                          {
                            label: "DEA",
                            color: "#e5b45a",
                            values: page.bars.map(bar => bar.macd?.signal ?? null),
                          },
                        ],
                        histogram: page.bars.map(bar => bar.macd?.histogram ?? null),
                      }
                    : undefined
                }
                key={`${id}:${page.offset}:${page.manifest_sha256}`}
                series={{ kind: "candles", points: chartBars }}
                label={t("合约历史 K 线")}
                volumeLabel={t("成交量")}
                formatTime={formatTime}
                formatAxisTime={daily ? dailyChartDate : ns => chartTime(ns, true)}
                describe={index => (
                  <>
                    {formatTime(chartBars[index].timestamp_ns)} · O {page.bars[index].open} · H{" "}
                    {page.bars[index].high} · L {page.bars[index].low} · C {page.bars[index].close}
                  </>
                )}
              />
              <div className="market-chart-toolbar market-history-pages">
                <span>
                  {page.offset + 1}–{page.offset + page.bars.length} / {page.matched_rows}
                </span>
                <button disabled={disabled || !page.offset} onClick={() => setOffset(0)}>
                  {t("最早数据")}
                </button>
                <button
                  disabled={disabled || !page.offset}
                  onClick={() => setOffset(Math.max(0, page.offset - 100))}
                >
                  {t("向前")}
                </button>
                <button
                  disabled={disabled || page.offset + page.bars.length >= page.matched_rows}
                  onClick={() => setOffset(page.offset + 100)}
                >
                  {t("向后")}
                </button>
                <button
                  disabled={disabled || page.offset + page.bars.length >= page.matched_rows}
                  onClick={() => setOffset(Math.floor((page.matched_rows - 1) / 100) * 100)}
                >
                  {t("最新数据")}
                </button>
              </div>
              <div className="market-chart-source">
                {page.source === "tushare.ft_mins" || page.source === "tushare.fut_daily"
                  ? "Tushare"
                  : page.source}{" "}
                · {t("历史数据 · 非实时")}
              </div>
            </>
          ) : (
            <div className="market-chart-empty">{t(loading ? "读取中" : "暂无历史记录")}</div>
          )}
        </>
      ) : (
        <div className="market-chart-empty">
          <p>{t("该月份合约尚无已下载历史数据")}</p>
          <button onClick={() => navigate("workspace.data", { page: "history" })}>
            {t("下载历史数据")}
          </button>
        </div>
      )}
    </section>
  );
}
