import { dailyChartBar, dailyChartDate } from "../../src/ui/charts/daily-chart";
import { PriceChart } from "@asterion/client-ui/PriceChart";
import { useCallback, useEffect, useRef, useState } from "react";
import {
  timestamp,
  type HistoryBar,
  type HistoryPage,
  type DailyPage,
  type DailyBar,
} from "../../src/bridge/client";
import {
  type TerminalContext,
  translate,
  getLocale,
  asDisplayError,
  ErrorNotice,
  type DisplayError,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
const wall = (value: string) =>
  value ? value.replace("T", " ") + (value.length === 16 ? ":00" : "") : "";
function BarChart({
  bars,
  formatTime,
}: {
  bars: HistoryBar[];
  formatTime: (value: string) => string;
}) {
  return (
    <>
      <PriceChart
        series={{ kind: "candles", points: bars }}
        label={t("合约 K 线与成交量")}
        volumeLabel={t("成交量")}
        formatTime={formatTime}
        describe={index => {
          const bar = bars[index];
          return (
            <>
              {formatTime(bar.timestamp_ns)} · {t("开")} {bar.open} · {t("高")} {bar.high} ·{" "}
              {t("低")} {bar.low} · {t("收")} {bar.close} · {t("量")} {bar.volume}
            </>
          );
        }}
      />
      <p className="subtle">{t("图表操作提示")}</p>
    </>
  );
}
export function HistoryDatasetViewer({
  id,
  archive = false,
  source,
  sourceLabel,
  timeAxis,
  snapshot,
  busy,
  query,
  onClose,
}: Pick<TerminalContext, "snapshot" | "busy" | "query"> & {
  id: string;
  archive?: boolean;
  source: string;
  sourceLabel: string;
  timeAxis: "instant" | "trading-day";
  onClose: () => void;
}) {
  const [error, setError] = useState<DisplayError>("");
  const [filter, setFilter] = useState({ start: "", end: "" });
  const [applied, setApplied] = useState({ start: "", end: "" });
  const [page, setPage] = useState<HistoryPage | DailyPage | null>(null);
  const [loading, setLoading] = useState(false);
  const sequence = useRef(0);
  const invoke = useRef(query);
  useEffect(() => {
    invoke.current = query;
  }, [query]);
  const daily = timeAxis === "trading-day";
  const formatTime = daily ? dailyChartDate : timestamp;
  const offline = !snapshot?.research?.online;
  const disabled = busy || loading || offline;
  const fetchPage = useCallback(
    async (offset: number, limit: number, range: { start: string; end: string }) => {
      const current = ++sequence.current;
      setError("");
      setLoading(true);
      try {
        const result = await invoke.current(
          daily ? "research.daily.page" : "research.minutes.page",
          {
            ...(daily ? { period: "day" } : {}),
            include_macd: false,
            archive,
            id,
            offset,
            limit,
            start: daily ? range.start : wall(range.start),
            end: daily ? range.end : wall(range.end),
          },
        );
        if (current !== sequence.current) return;
        const next = daily ? result.daily_page : result.history_page;
        if (
          !next ||
          next.id !== id ||
          next.source !== source ||
          (daily && (!("period" in next) || next.period !== "day"))
        )
          throw new Error(t("返回的数据与所选数据集不匹配"));
        setPage(next);
        setApplied(range);
        setFilter(range);
      } catch (reason) {
        if (current === sequence.current) setError(asDisplayError(reason));
      } finally {
        if (current === sequence.current) setLoading(false);
      }
    },
    [id, source, daily, archive],
  );
  const cancelPending = useCallback(() => {
    sequence.current++;
  }, []);
  useEffect(() => {
    setPage(null);
    void fetchPage(0, 100, { start: "", end: "" });
    return cancelPending;
  }, [fetchPage, cancelPending]);
  const load = (offset: number, limit: number, range = applied) => fetchPage(offset, limit, range);
  const rows: (HistoryBar & Partial<DailyBar>)[] =
    page?.bars.map(bar => ("trading_day" in bar ? dailyChartBar(bar) : bar)) ?? [];
  const first = page ? ("first_day" in page ? page.first_day : timestamp(page.first_ns)) : "";
  const last = page ? ("last_day" in page ? page.last_day : timestamp(page.last_ns)) : "";
  const begin = page ? ("begin_day" in page ? page.begin_day : timestamp(page.begin_ns)) : "";
  const end = page ? ("end_day" in page ? page.end_day : timestamp(page.end_ns)) : "";
  return (
    <section className="history-viewer" aria-label={t("历史数据查看")} aria-busy={loading}>
      <div className="history-viewer-heading">
        <button onClick={onClose}>{t("返回数据集")}</button>
        <strong>{page?.contract_id ?? id}</strong>
        <span>
          {daily
            ? t("日K")
            : page && "interval_minutes" in page
              ? `${page.interval_minutes} min`
              : ""}{" "}
          · {sourceLabel}
        </span>
      </div>
      {offline && (
        <p className="subtle" role="status">
          {t("历史数据服务未连接，查询暂不可用。")}
          {page && ` ${t("已加载的图表与记录仍可查看。")}`}
        </p>
      )}
      {error && (
        <div role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.data-workbench" />
        </div>
      )}
      {!page ? (
        <div>
          <p>{t(loading ? "读取中" : "尚未加载数据")}</p>
          {!loading && (
            <button disabled={disabled} onClick={() => void load(0, 100)}>
              {t("查看数据")}
            </button>
          )}
        </div>
      ) : (
        <>
          <dl className="history-metrics">
            <div>
              <dt>{t("总记录数")}</dt>
              <dd>{page.total_rows.toLocaleString(getLocale())}</dd>
            </div>
            <div>
              <dt>{t(daily ? "首个交易日期" : "实际首条（北京时间）")}</dt>
              <dd>{page.total_rows ? first : "—"}</dd>
            </div>
            <div>
              <dt>{t(daily ? "末个交易日期" : "实际末条（北京时间）")}</dt>
              <dd>{page.total_rows ? last : "—"}</dd>
            </div>
            <div>
              <dt>{t("下载状态")}</dt>
              <dd>
                {t("已完成")}
                {offline && ` · ${t("最后确认状态")}`}
              </dd>
            </div>
          </dl>
          <form
            className="history-view-controls"
            onSubmit={event => {
              event.preventDefault();
              void load(0, page.limit, filter);
            }}
          >
            <label>
              {t(daily ? "开始交易日期" : "开始时间（北京时间）")}
              <input
                type={daily ? "date" : "datetime-local"}
                value={filter.start}
                onChange={event =>
                  setFilter(previous => ({ ...previous, start: event.target.value }))
                }
              />
            </label>
            <label>
              {t(daily ? "结束交易日期" : "结束时间（北京时间）")}
              <input
                type={daily ? "date" : "datetime-local"}
                min={filter.start}
                value={filter.end}
                onChange={event =>
                  setFilter(previous => ({ ...previous, end: event.target.value }))
                }
              />
            </label>
            <button disabled={disabled} type="submit">
              {t("查看区间")}
            </button>
            <button
              disabled={disabled}
              type="button"
              onClick={() => void load(0, page.limit, { start: "", end: "" })}
            >
              {t("全部时间")}
            </button>
            <label>
              {t("图表窗口")}
              <select
                aria-label={t("图表窗口")}
                disabled={disabled}
                value={page.limit}
                onChange={event => {
                  const limit = Number(event.target.value);
                  void load(Math.floor(page.offset / limit) * limit, limit);
                }}
              >
                {[25, 50, 100, 200].map(size => (
                  <option key={size} value={size}>
                    {size}
                  </option>
                ))}
              </select>
            </label>
          </form>
          <p className="subtle">
            {t("当前筛选范围")} · {begin} — {end}
          </p>
          {page.bars.length ? (
            <BarChart
              key={`${page.id}:${page.offset}:${page.limit}:${begin}:${end}`}
              bars={rows}
              formatTime={formatTime}
            />
          ) : (
            <p className="history-empty">{t("此范围没有记录；不能据此判定缺失或无成交。")}</p>
          )}
          <div className="history-pagination">
            <span>
              {page.matched_rows ? page.offset + 1 : 0}–{page.offset + page.bars.length} /{" "}
              {page.matched_rows.toLocaleString(getLocale())} {t("根 K 线")}
            </span>
            <button disabled={disabled || !page.offset} onClick={() => void load(0, page.limit)}>
              {t("最早数据")}
            </button>
            <button
              disabled={disabled || !page.offset}
              onClick={() => void load(Math.max(0, page.offset - page.limit), page.limit)}
            >
              {t("上一页")}
            </button>
            <button
              disabled={disabled || page.offset + page.bars.length >= page.matched_rows}
              onClick={() => void load(page.offset + page.limit, page.limit)}
            >
              {t("下一页")}
            </button>
            <button
              disabled={disabled || page.offset + page.bars.length >= page.matched_rows}
              onClick={() =>
                void load(
                  Math.floor(Math.max(0, page.matched_rows - 1) / page.limit) * page.limit,
                  page.limit,
                )
              }
            >
              {t("最新数据")}
            </button>
          </div>
          <div
            className="history-bars-table"
            tabIndex={0}
            role="region"
            aria-label={t(daily ? "日线数据表" : "分钟数据表")}
          >
            <table className="data-table">
              <thead>
                <tr>
                  {[
                    daily ? "交易日期" : "时间（北京时间）",
                    "开盘",
                    "最高",
                    "最低",
                    "收盘",
                    "成交量",
                    "成交额",
                    "持仓量",
                    ...(daily ? ["昨收", "昨结算", "结算价"] : []),
                  ].map((key, index) => (
                    <th className={index ? "numeric" : undefined} key={key}>
                      {t(key)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.map(bar => (
                  <tr key={bar.timestamp_ns}>
                    <td>{formatTime(bar.timestamp_ns)}</td>
                    {[
                      bar.open,
                      bar.high,
                      bar.low,
                      bar.close,
                      bar.volume,
                      bar.amount,
                      bar.open_interest,
                      ...("settlement" in bar
                        ? [
                            bar.previous_close ?? "—",
                            bar.previous_settlement ?? "—",
                            bar.settlement ?? "—",
                          ]
                        : []),
                    ].map((value, index) => (
                      <td className="numeric" key={index}>
                        {value}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <details className="history-notes">
            <summary>{t("质量与来源")}</summary>
            <p>
              {t(
                daily
                  ? "清单与日线分段摘要、交易日期顺序及 OHLC 关系已校验。"
                  : "清单与已读取分段的摘要、时间顺序、重复时间戳及 OHLC 关系已校验。",
              )}
            </p>
            <p>{t("未校验交易日历完整性；缺失、休市与无成交尚不能自动区分。")}</p>
            <p>{t("价格与数量保留精确原值；图表仅用于可视化。")}</p>
            <p>
              {t("内容版本")} <code>{page.manifest_sha256}</code>
            </p>
          </details>
        </>
      )}
    </section>
  );
}
