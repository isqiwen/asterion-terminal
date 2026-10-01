import { useState } from "react";
import { getLocale, translate, type MessageValues } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { ResearchTask, Snapshot, TerminalCommand } from "../bridge/client";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

const download = (task: ResearchTask) =>
  task.state === "succeeded" && (task.kind === "minute_download" || task.kind === "daily_download");
const label = (task: ResearchTask) =>
  t("{instrument} · {period} · {source} · {time}", {
    instrument: task.instrument,
    time: new Date(task.updated_at_ms).toLocaleString(getLocale(), { hour12: false }),
    period:
      task.kind === "daily_download"
        ? t("日线")
        : t("{n} 分钟", { n: task.minute_interval_minutes ?? 1 }),
    source: task.data_source || task.source_name,
  });

// Mirrors the core's portfolio limit.
const maxContracts = 20;

// Chooses bars from completed data-source downloads for paper trading,
// backtests, factors and strategies; a portfolio holds one dataset per
// contract and all of them share the same trading days. The contract identity
// comes from the download; only units a data source does not provide are
// entered here.
export function DatasetPicker({
  snapshot,
  busy,
  trade,
  locked = false,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
  locked?: boolean;
}) {
  const selected = snapshot?.datasets ?? [];
  // Newest first, so the default settlement is the latest completed download.
  const tasks = (snapshot?.research?.tasks ?? [])
    .filter(download)
    .sort((left, right) => right.updated_at_ms - left.updated_at_ms);
  const [source, setSource] = useState("");
  const [settlement, setSettlement] = useState("");
  const [range, setRange] = useState({ begin_day: "", end_day: "" });
  const [units, setUnits] = useState({ price_increment: "", multiplier: "" });
  const [error, setError] = useState<DisplayError>("");
  const chosen = tasks.find(task => task.id === source);
  const settlements = tasks.filter(
    task => task.kind === "daily_download" && task.instrument === chosen?.instrument,
  );
  function choose(id: string) {
    setSource(id);
    const task = tasks.find(item => item.id === id);
    const daily = tasks.find(
      item => item.kind === "daily_download" && item.instrument === task?.instrument,
    );
    setSettlement(daily?.id ?? "");
    // CTP's contract catalog, when connected, supplies the exchange units.
    const listed = snapshot?.market?.catalog.contracts.find(
      contract => contract.contract_id === task?.instrument,
    );
    if (listed)
      setUnits({ price_increment: listed.price_tick, multiplier: String(listed.multiplier) });
  }
  async function run(method: TerminalCommand, params: Record<string, unknown> = {}) {
    setError("");
    try {
      await trade(method, params);
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  const list = (
    <ul className="dataset-list" aria-label={t("已选合约")}>
      {selected.map(item => (
        <li key={`${item.venue}.${item.symbol}`}>
          <p>
            <strong>
              {item.venue} · {item.symbol}
            </strong>{" "}
            ·{" "}
            {item.interval_minutes === 1440
              ? t("日线")
              : t("{n} 分钟", { n: item.interval_minutes })}{" "}
            · {t("{count} 根 · {days} 个交易日", { count: item.count, days: item.days })}
          </p>
          <p className="subtle">
            {item.first_day} → {item.last_day} · {item.source} ·{" "}
            {t("最小变动价位 {tick} · 合约乘数 {multiplier}", {
              tick: item.contract.price_increment,
              multiplier: item.contract.multiplier,
            })}
          </p>
          <p className="subtle">
            {t("数据版本")} <code>{item.revision.slice(0, 16)}</code>
          </p>
          {item.uncovered_days.length > 0 && (
            <p role="alert" className="alert">
              {t("日线中有 {n} 个交易日没有分钟数据：{days}", {
                n: item.uncovered_days.length,
                days:
                  item.uncovered_days.slice(0, 5).join("、") +
                  (item.uncovered_days.length > 5 ? " …" : ""),
              })}
            </p>
          )}
          {!locked && (
            <div className="source-actions">
              <button
                type="button"
                disabled={busy}
                aria-label={t("移除 {contract}", { contract: `${item.venue} · ${item.symbol}` })}
                onClick={() =>
                  void run("research.dataset.remove", { venue: item.venue, symbol: item.symbol })
                }
              >
                {t("移除")}
              </button>
            </div>
          )}
        </li>
      ))}
    </ul>
  );
  if (locked || selected.length >= maxContracts)
    return (
      <section className="dataset-picker" aria-label={t("历史数据集")}>
        <h3>{t("历史数据集")}</h3>
        {list}
        {!locked && <p className="subtle">{t("组合最多 {n} 个合约。", { n: maxContracts })}</p>}
        {error && (
          <p role="alert" className="alert">
            <ErrorNotice error={error} />
          </p>
        )}
      </section>
    );
  return (
    <form
      className="dataset-picker"
      aria-label={t("历史数据集")}
      onSubmit={event => {
        event.preventDefault();
        void run("research.dataset.select", {
          source_task_id: source,
          settlement_task_id: settlement,
          ...range,
          ...units,
        });
      }}
    >
      <h3>{t("历史数据集")}</h3>
      {list}
      {!snapshot?.research ? (
        <p className="subtle">{t("研究服务未连接，无法读取已下载的历史数据。")}</p>
      ) : !tasks.length ? (
        <p className="subtle">
          {t("还没有已完成的历史下载。请先在数据页从数据源下载分钟线或日线。")}
        </p>
      ) : (
        <fieldset disabled={busy}>
          <div className="futures-fields">
            <label>
              {t("K 线来源")}
              <select
                aria-label={t("K 线来源")}
                value={source}
                onChange={event => choose(event.target.value)}
                required
              >
                <option value="">{t("选择已完成的下载")}</option>
                {tasks.map(task => (
                  <option key={task.id} value={task.id}>
                    {label(task)}
                  </option>
                ))}
              </select>
            </label>
            <label>
              {t("结算价来源")}
              <select
                aria-label={t("结算价来源")}
                value={settlement}
                onChange={event => setSettlement(event.target.value)}
                required
              >
                <option value="">
                  {chosen && !settlements.length ? t("需要同一合约的日线下载") : t("选择日线下载")}
                </option>
                {settlements.map(task => (
                  <option key={task.id} value={task.id}>
                    {label(task)}
                  </option>
                ))}
              </select>
            </label>
            <label>
              {t("开始交易日")}
              <input
                aria-label={t("开始交易日")}
                type="date"
                value={range.begin_day}
                onChange={event => setRange({ ...range, begin_day: event.target.value })}
              />
            </label>
            <label>
              {t("结束交易日")}
              <input
                aria-label={t("结束交易日")}
                type="date"
                value={range.end_day}
                onChange={event => setRange({ ...range, end_day: event.target.value })}
              />
            </label>
            <label>
              {t("最小变动价位")}
              <input
                aria-label={t("最小变动价位")}
                inputMode="decimal"
                value={units.price_increment}
                onChange={event => setUnits({ ...units, price_increment: event.target.value })}
                required
              />
            </label>
            <label>
              {t("合约乘数")}
              <input
                aria-label={t("合约乘数")}
                inputMode="decimal"
                value={units.multiplier}
                onChange={event => setUnits({ ...units, multiplier: event.target.value })}
                required
              />
            </label>
          </div>
          <div className="source-actions">
            <button type="submit" className="primary">
              {selected.length ? t("加入组合") : t("使用此数据集")}
            </button>
            <span className="subtle">
              {t("交易日区间留空表示全部；组合内合约须覆盖相同交易日；合约单位以交易所公布为准。")}
            </span>
          </div>
        </fieldset>
      )}
      {error && (
        <p role="alert" className="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </form>
  );
}
