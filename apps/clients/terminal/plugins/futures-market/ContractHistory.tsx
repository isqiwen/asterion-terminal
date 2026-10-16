import type { DailyTable } from "../../src/ui/useDailyTable";
import { dailyChartBar, dailyChartDate } from "../../src/ui/charts/daily-chart";
import { chartTime } from "./chart-time";
import { averagePeriods, historyLookback, movingAverage } from "./indicators";
import { useEffect, useRef, useState } from "react";
import { PriceChart } from "@asterion/client-ui/PriceChart";
import { timestamp, type HistoryBar, type HistoryDatasetRecord } from "../../src/bridge/client";
import {
  translate,
  useDailyTable,
  useWorkspaceDraft,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
// What one request drew: bars from the offset given of the total there are.
type Drawn = {
  offset: number;
  total: number;
  bars: (Pick<HistoryBar, "timestamp_ns" | "open" | "high" | "low" | "close" | "volume"> & {
    macd?: HistoryBar["macd"];
  })[];
  identity: string;
  source: string;
};

export function ContractHistory({
  venue,
  symbol,
  historyContractId,
  context,
  preferLongest = false,
  preferenceKey = preferLongest ? "history-long" : "history-main",
  fixedPeriod,
  dailyTable,
  fixedMinuteVersion,
}: {
  venue: string;
  symbol: string;
  historyContractId?: string;
  dailyTable?: DailyTable;
  // Parent fixes an immutable minute version for an explicit source segment.
  fixedMinuteVersion?: string;
  context: TerminalContext;
  preferLongest?: boolean;
  preferenceKey?: string;
  // Chosen by an enclosing period tab; hides this chart's own period controls.
  fixedPeriod?: number | "day" | "week" | "month" | "quarter" | "year";
}) {
  const { snapshot, query, busy, navigate } = context;
  const contractId =
    historyContractId ??
    snapshot?.market?.catalog.contracts.find(
      item => item.venue === venue && item.symbol.toUpperCase() === symbol.toUpperCase(),
    )?.contract_id;
  const connection = snapshot?.data?.connection_id;
  const online = !!snapshot?.data?.online;
  const publications = (snapshot?.task_service?.tasks ?? [])
    .filter(task => task.history_dataset_id)
    .map(task => task.history_dataset_id)
    .join(",");
  const [catalog, setCatalog] = useState<{ key: string; items: HistoryDatasetRecord[] }>({
    key: "",
    items: [],
  });
  const [retry, setRetry] = useState(0);
  const [catalogError, setCatalogError] = useState<DisplayError>("");
  const catalogKey = JSON.stringify([connection, contractId]);
  const queryCatalog = useRef(query);
  queryCatalog.current = query;
  useEffect(() => {
    let current = true;
    if (!contractId || !online) return;
    setCatalogError("");
    void queryCatalog
      .current("data.datasets", { venue: "", product: "", contract_id: contractId, source: "" })
      .then(result => {
        if (current) setCatalog({ key: catalogKey, items: result.history_datasets! });
      })
      .catch(reason => {
        if (current) setCatalogError(asDisplayError(reason));
      });
    return () => {
      current = false;
    };
  }, [catalogKey, contractId, online, publications, retry]);
  // Minutes are this contract's downloads; its days are read from the
  // market-wide daily table, under the snapshot the coverage names.
  const minutes =
    catalog.key === catalogKey ? catalog.items.filter(task => task.interval_minutes > 0) : [];
  const periods = [...new Set(minutes.map(task => task.interval_minutes))].sort((a, b) => a - b);
  const table = useDailyTable(snapshot, query, dailyTable);
  const held = table.held?.table;
  const [selection, setSelection] = useWorkspaceDraft<{
    connection: string | undefined;
    contract: string | undefined;
    id: string;
  }>(`${preferenceKey}.dataset`, { connection, contract: contractId, id: "" });
  const selected =
    selection.connection === connection && selection.contract === contractId ? selection.id : "";
  const setSelected = (id: string) => setSelection({ connection, contract: contractId, id });
  // The chart pane keeps its display choices when a different contract is selected.
  // Dataset IDs and paging remain contract-local; unavailable periods never fall back.
  const [period, setPeriod] = useWorkspaceDraft<
    number | "day" | "week" | "month" | "quarter" | "year" | null
  >(`${preferenceKey}.period`, null);
  // Chosen once both the downloads and the daily table are known.
  const known = catalog.key === catalogKey && !table.loading;
  const initialPeriod = !known
    ? undefined
    : preferLongest
      ? held
        ? "day"
        : periods.at(-1)
      : (minutes[0]?.interval_minutes ?? (held ? "day" : undefined));
  const activePeriod = fixedPeriod ?? period ?? initialPeriod;
  useEffect(() => {
    // A tab-imposed period is not this chart's own remembered choice.
    if (fixedPeriod === undefined && period === null && activePeriod !== undefined)
      setPeriod(activePeriod);
  }, [fixedPeriod, period, activePeriod, setPeriod]);
  const daily = typeof activePeriod === "string";
  const id = daily
    ? ""
    : (minutes.find(
        task =>
          task.id === (fixedMinuteVersion ?? selected) && task.interval_minutes === activePeriod,
      )?.id ??
      (fixedMinuteVersion === undefined
        ? minutes.find(task => task.interval_minutes === activePeriod)?.id
        : undefined) ??
      "");
  // What is drawn: a minute download, or the contract in the daily table.
  const source = daily ? (held && contractId ? JSON.stringify([held, contractId]) : "") : id;
  const datasetKey = JSON.stringify([connection, source, activePeriod]);
  // Keep the last page in this chart pane when navigating to research/data.
  // An unresolved catalog must not overwrite it during remount. A different
  // service, contract, version, snapshot or period never inherits its offset.
  const [paging, setPaging] = useWorkspaceDraft(`${preferenceKey}.paging`, { key: "", offset: 0 });
  const offset = paging.key === datasetKey ? paging.offset : 0;
  const setOffset = (offset: number) => setPaging({ key: datasetKey, offset });
  const [showMacd, setShowMacd] = useWorkspaceDraft(`${preferenceKey}.macd`, true);
  const [showAverages, setShowAverages] = useWorkspaceDraft(`${preferenceKey}.averages`, true);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [response, setResponse] = useState<{ key: string; page: Drawn } | null>(null);
  const invoke = useRef(query);
  useEffect(() => {
    invoke.current = query;
  }, [query]);
  useEffect(() => {
    let current = true;
    setError("");
    if (!source || !online) {
      setLoading(false);
      return;
    }
    setLoading(true);
    const from = Math.max(0, offset - historyLookback);
    const limit = 100 + historyLookback;
    void (
      daily
        ? invoke
            .current("data.tables.daily", {
              // A daily source is formed only with held and contractId.
              daily: held!,
              contract: contractId!,
              period: activePeriod,
              offset: from,
              limit,
            })
            .then((result): Drawn => {
              const page = result.table_daily;
              if (
                !page ||
                page.contract !== contractId ||
                page.period !== activePeriod ||
                page.daily.snapshot !== held?.snapshot
              )
                throw new Error(t("返回的数据与所选数据集不匹配"));
              return {
                offset: page.offset,
                total: page.total,
                bars: page.candles.map(dailyChartBar),
                identity: `${page.daily.snapshot}:${page.contract}`,
                source: t("全市场日线表"),
              };
            })
        : invoke
            .current("data.minutes.page", {
              include_macd: true,
              id,
              offset: from,
              limit,
              start: "",
              end: "",
            })
            .then((result): Drawn => {
              const page = result.history_page;
              if (
                !page ||
                page.id !== id ||
                page.source !== "tushare.ft_mins" ||
                page.contract_id !== contractId
              )
                throw new Error(t("返回的数据与所选数据集不匹配"));
              return {
                offset: page.offset,
                total: page.matched_rows,
                bars: page.bars,
                identity: `${page.id}:${page.manifest_sha256}`,
                source: "Tushare",
              };
            })
    )
      .then(page => {
        if (current) setResponse({ key: datasetKey, page });
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
  }, [datasetKey, source, id, held, contractId, offset, online, retry, daily, activePeriod]);
  const rawPage = response?.key === datasetKey ? response.page : null;
  const leading = rawPage?.offset ? historyLookback : 0;
  const page = rawPage
    ? {
        ...rawPage,
        offset: rawPage.offset + leading,
        bars: rawPage.bars.slice(leading, leading + 100),
      }
    : null;
  const chartBars = page?.bars ?? [];
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
      {(catalogError || table.error || error) && (
        <div role="alert">
          <ErrorNotice
            error={catalogError || table.error || error}
            namespace="asterion.terminal.futures-market"
          />
          <button disabled={disabled} onClick={() => setRetry(value => value + 1)}>
            {t("重试")}
          </button>
        </div>
      )}
      {catalogError ? null : !online && !page ? (
        <div className="market-chart-empty" role="status">
          <p>{t("历史数据服务未连接")}</p>
          <button onClick={() => navigate("workspace.data", { page: "history" })}>
            {t("管理历史数据")}
          </button>
        </div>
      ) : minutes.length || held || period !== null || fixedPeriod !== undefined ? (
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
                      setSelected(minutes.find(task => task.interval_minutes === period)!.id);
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
                  disabled={disabled || !held}
                  title={held ? undefined : t("全市场日线表还没有入库")}
                  onClick={() => {
                    setPeriod(value);
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
            {source && (
              <details className="market-dataset-choice">
                <summary>{t("数据版本")}</summary>
                {daily ? (
                  <p>
                    {t("全市场日线表")} · <code>{held!.snapshot.slice(0, 12)}</code>
                  </p>
                ) : fixedMinuteVersion !== undefined ? (
                  <p>
                    {t("固定分钟档案版本")} · <code>{fixedMinuteVersion}</code>
                  </p>
                ) : (
                  <select
                    aria-label={t("历史数据版本")}
                    value={id}
                    onChange={event => {
                      setSelected(event.target.value);
                      const chosen = minutes.find(task => task.id === event.target.value);
                      setPeriod(chosen ? chosen.interval_minutes : null);
                      setOffset(0);
                    }}
                    disabled={disabled}
                  >
                    {minutes.map(task => (
                      <option key={task.id} value={task.id}>
                        {task.source} · {task.begin} — {task.end} · {task.id.slice(0, 8)}
                      </option>
                    ))}
                  </select>
                )}
                <p className="subtle">
                  {t("MACD 从数据集首根收盘价起算，前 33 根为预热；图形指标不用于交易执行。")}
                </p>
                {daily && (
                  <p className="subtle">
                    {t(
                      "日线只画合约在集合竞价中有成交的交易日：没有成交的交易日、全市场日线表没有入库的交易日都不画，也不补。",
                    )}
                  </p>
                )}
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
          {!online && <p className="subtle">{t("历史数据服务未连接")}</p>}
          {!source ? (
            daily && contractId ? (
              <div className="market-chart-empty" role="status">
                <p>{t(table.loading ? "读取中" : "全市场日线表还没有入库")}</p>
                {!table.loading && (
                  <button onClick={() => navigate("workspace.data", { page: "tables" })}>
                    {t("更新全市场数据")}
                  </button>
                )}
              </div>
            ) : (
              <div className="market-chart-empty" role="status">
                <p>
                  {t(minutes.length ? "该合约尚无所选周期的数据" : "该月份合约尚无已下载历史数据")}
                </p>
                <button onClick={() => navigate("workspace.data", { page: "history" })}>
                  {t("下载历史数据")}
                </button>
              </div>
            )
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
                key={`${page.identity}:${page.offset}`}
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
                  {page.offset + 1}–{page.offset + page.bars.length} / {page.total}
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
                  disabled={disabled || page.offset + page.bars.length >= page.total}
                  onClick={() => setOffset(page.offset + 100)}
                >
                  {t("向后")}
                </button>
                <button
                  disabled={disabled || page.offset + page.bars.length >= page.total}
                  onClick={() => setOffset(Math.floor((page.total - 1) / 100) * 100)}
                >
                  {t("最新数据")}
                </button>
              </div>
              <div className="market-chart-source">
                {page.source} · {t("历史数据 · 非实时")}
              </div>
            </>
          ) : rawPage && !rawPage.total ? (
            <div className="market-chart-empty" role="status">
              <p>
                {t(daily ? "全市场日线表里没有这个合约在集合竞价中成交的交易日" : "暂无历史记录")}
              </p>
              {daily && (
                <button onClick={() => navigate("workspace.data", { page: "tables" })}>
                  {t("更新全市场数据")}
                </button>
              )}
            </div>
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
