import { SaveSelection, SavedDatasets } from "./SavedDatasets";
import { useEffect, useState } from "react";
import { translate, type MessageValues } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { HistoryDatasetRecord, Snapshot, TerminalCommand } from "../bridge/client";
import { useHistoryDatasets, type HistoryQuery } from "./useHistoryDatasets";
import { useDailyTable, type DailyTableQuery } from "./useDailyTable";
import { useWorkspaceDraft } from "../host/workspace/drafts";
import { DominantSeriesPicker } from "./DominantSeriesPicker";
import { DailyBarsPicker } from "./DailyBarsPicker";
import { DailyTableGaps } from "./DailyTableGaps";
import { RollSchedule } from "./RollSchedule";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// A minute download, as the lists of them name it.
const label = (item: HistoryDatasetRecord) =>
  `${item.contract_id} · ${t("{n} 分钟", { n: item.interval_minutes })} · ${item.begin.slice(0, 10)} — ${item.end.slice(0, 10)} · ${item.source} · ${item.id.slice(0, 8)}`;

// Mirrors the core's portfolio limit: what it trades at a time, each contract
// and each dominant series as one.
const maxMembers = 20;

// One way of adding data is shown at a time, each a form of its own.
const methods = [
  ["daily", "日线合约"],
  ["daily-series", "日线主力连续"],
  ["minute", "分钟线合约"],
  ["minute-series", "分钟线主力连续"],
  ["saved", "已保存数据集"],
] as const;

// Chooses the data of backtests and factors; a portfolio holds one dataset per
// contract and all of them share the same trading days. Daily bars are the
// market-wide daily table's own; minute bars come from published archive
// versions, whose contract identity is the archive's. Only units a data source
// does not provide are entered here. Settlement prices and trading days are
// always the daily table's, under the snapshot read when the picker opened.
export function DatasetPicker({
  snapshot,
  busy,
  trade,
  query,
  locked = false,
  sourceRequest,
  onDownload,
  onSelected,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
  query: HistoryQuery & DailyTableQuery;
  locked?: boolean;
  sourceRequest?: { id: string; connection: string; request: string };
  onDownload?: () => void;
  onSelected?: () => void;
}) {
  const series = snapshot?.dataset_series ?? [];
  // Months of a dominant series are shown with their series, not one by one.
  const inSeries = (item: { venue: string; symbol: string }) =>
    series.some(group => group.venue === item.venue && group.symbols.includes(item.symbol));
  const all = snapshot?.datasets ?? [];
  const selected = all.filter(item => !inSeries(item));
  const members = selected.length + series.length;
  const { items: archived, error: loadError, loading } = useHistoryDatasets(snapshot, query);
  // Minute bars are chosen from downloads. Daily bars are the market-wide
  // daily table's own, so a download of them is not a source.
  const versions = archived.filter(item => item.interval_minutes > 0);
  const daily = useDailyTable(snapshot, query);
  const [draft, setDraft] = useWorkspaceDraft(
    `dataset-picker:composition:${snapshot?.data?.connection_id}`,
    {
      source: "",
      extraSources: [] as string[],
      begin_day: "",
      end_day: "",
      price_increment: "",
      multiplier: "",
      consumed: "",
    },
  );
  const { source, extraSources, begin_day, end_day, price_increment, multiplier } = draft;
  // What is already chosen stays above every way of adding to it.
  const [method, setMethod] = useWorkspaceDraft<(typeof methods)[number][0]>(
    "dataset-picker:method",
    "daily",
  );
  const [savedRevision, setSavedRevision] = useState(0);
  const [error, setError] = useState<DisplayError>("");
  const chosen = versions.find(item => item.id === source);
  const replacing = selected.some(
    item =>
      `${item.contract.venue}/${item.contract.product.toLowerCase()}/${item.contract.delivery_month}` ===
      chosen?.contract_id,
  );
  // What the daily table holds of the chosen contract's exchange.
  const exchange = chosen?.contract_id.split("/")[0];
  const settled = daily.held?.stored.find(row => row.exchange === exchange);
  const additionalSources = versions.filter(
    item =>
      item.id !== source &&
      item.contract_id === chosen?.contract_id &&
      item.interval_minutes === chosen?.interval_minutes &&
      item.source === chosen?.source,
  );
  function toggle(id: string) {
    setDraft(previous => ({
      ...previous,
      extraSources: previous.extraSources.includes(id)
        ? previous.extraSources.filter(value => value !== id)
        : [...previous.extraSources, id],
    }));
  }
  function selection(id: string) {
    const item = versions.find(item => item.id === id);
    const listed = snapshot?.market?.catalog.contracts.find(
      contract => contract.contract_id === item?.contract_id,
    );
    // Never reuse another contract's manually entered exchange units or dates.
    return {
      source: item?.id ?? "",
      extraSources: [],
      begin_day: "",
      end_day: "",
      price_increment: listed?.price_tick ?? "",
      multiplier: listed ? String(listed.multiplier) : "",
    };
  }
  function choose(id: string) {
    setError("");
    setDraft(previous => ({ ...previous, ...selection(id) }));
  }
  useEffect(() => {
    if (
      loading ||
      !sourceRequest ||
      !snapshot?.data?.online ||
      draft.consumed === sourceRequest.request
    )
      return;
    const valid =
      sourceRequest.connection === snapshot.data.connection_id &&
      versions.some(item => item.id === sourceRequest.id);
    setDraft(previous => ({
      ...previous,
      ...selection(valid ? sourceRequest.id : ""),
      consumed: sourceRequest.request,
    }));
    setError(valid ? "" : t("数据版本不属于当前数据服务，请重新选择数据。"));
    // The version asked for is a minute download.
    setMethod("minute");
    // Consume each navigation intent once; subsequent edits belong to this draft.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [
    loading,
    sourceRequest?.request,
    snapshot?.data?.connection_id,
    snapshot?.data?.online,
    draft.consumed,
  ]);
  async function run(method: TerminalCommand, params: Record<string, unknown> = {}) {
    setError("");
    try {
      await trade(method, params);
      if (
        method === "data.dataset.select" ||
        method === "data.dataset.daily" ||
        method === "data.dataset.series" ||
        method === "data.dataset.product"
      )
        onSelected?.();
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
            {item.interval_minutes === 0 ? t("日线") : t("{n} 分钟", { n: item.interval_minutes })}{" "}
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
            {item.source_dataset_ids.length
              ? t("K 线 {bars} 份 · 结算价取自全市场日线表（快照 {snapshot}）", {
                  bars: item.source_dataset_ids.length,
                  snapshot: item.daily.snapshot.slice(0, 12),
                })
              : t("日线和结算价取自全市场日线表（快照 {snapshot}）", {
                  snapshot: item.daily.snapshot.slice(0, 12),
                })}{" "}
            · {t("数据版本")} <code>{item.revision.slice(0, 16)}</code>
          </p>
          {item.uncovered_days.length > 0 && (
            <p role="alert" className="alert">
              {t("结算日历中有 {n} 个交易日没有 K 线：{days}", {
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
                  void run("data.dataset.remove", { venue: item.venue, symbol: item.symbol })
                }
              >
                {t("移除")}
              </button>
            </div>
          )}
        </li>
      ))}
      {series.map(group => {
        const months = all.filter(
          item => item.venue === group.venue && group.symbols.includes(item.symbol),
        );
        const name = `${group.venue} · ${group.product} ${t("主力连续")}`;
        return (
          <li key={`${group.venue}.${group.product}`}>
            <p>
              <strong>{name}</strong> ·{" "}
              {months[0]?.interval_minutes === 0
                ? t("日线")
                : t("{n} 分钟", { n: months[0]?.interval_minutes ?? 0 })}{" "}
              ·{" "}
              {t("{count} 根 · {months} 个月份", {
                count: months.reduce((sum, item) => sum + item.count, 0),
                months: group.symbols.length,
              })}
            </p>
            <p className="subtle">
              {group.rolls[0]?.trading_day} → {months.at(-1)?.last_day} · {months[0]?.source} ·{" "}
              {t("最小变动价位 {tick} · 合约乘数 {multiplier}", {
                tick: months[0]?.contract.price_increment ?? "",
                multiplier: months[0]?.contract.multiplier ?? "",
              })}
            </p>
            <RollSchedule name={name} rolls={group.rolls} />
            {!locked && (
              <div className="source-actions">
                <button
                  type="button"
                  disabled={busy}
                  aria-label={t("移除 {contract}", { contract: name })}
                  onClick={() =>
                    void run("data.dataset.remove", {
                      venue: group.venue,
                      symbol: group.symbols[0],
                    })
                  }
                >
                  {t("移除")}
                </button>
              </div>
            )}
          </li>
        );
      })}
    </ul>
  );
  if (locked)
    return (
      <section className="dataset-picker" aria-label={t("历史数据集")}>
        <h3>{t("历史数据集")}</h3>
        {list}
        {error && (
          <p role="alert" className="alert">
            <ErrorNotice error={error} />
          </p>
        )}
      </section>
    );
  const data = snapshot?.data;
  return (
    <section className="dataset-picker" aria-label={t("历史数据集")}>
      <h3>{t("历史数据集")}</h3>
      <h4>{t("本次使用的数据")}</h4>
      {list}
      {all.length ? (
        <SaveSelection
          key={data?.connection_id}
          snapshot={snapshot}
          busy={busy}
          trade={trade}
          onSaved={() => setSavedRevision(value => value + 1)}
        />
      ) : (
        <p className="subtle">{t("尚未选择数据。用下面任一方式添加。")}</p>
      )}
      {members >= maxMembers && (
        <p className="subtle">
          {t("组合最多 {n} 个合约或主力连续，一条主力连续算一个。", { n: maxMembers })}
        </p>
      )}
      {loadError && (
        <p role="alert">
          <ErrorNotice error={loadError} />
        </p>
      )}
      {daily.error && (
        <p role="alert">
          <ErrorNotice error={daily.error} />
        </p>
      )}
      {loading && <p role="status">{t("正在读取历史仓库…")}</p>}
      <div className="activity-tabs" role="group" aria-label={t("添加数据的方式")}>
        {methods.map(([value, name]) => (
          <button
            key={value}
            type="button"
            aria-pressed={method === value}
            onClick={() => setMethod(value)}
          >
            {t(name)}
          </button>
        ))}
      </div>
      {!data && method !== "saved" && (
        <p className="subtle">{t("数据服务未连接，无法读取已下载的历史数据。")}</p>
      )}
      {data && (
        <>
          <div hidden={method !== "daily"}>
            <DailyBarsPicker
              key={data.connection_id}
              kind="contract"
              daily={daily.held}
              disabled={busy || !data.online || daily.loading}
              select={(command, params) => void run(command, params)}
            />
          </div>
          <div hidden={method !== "daily-series"}>
            <DailyBarsPicker
              key={data.connection_id}
              kind="product"
              daily={daily.held}
              disabled={busy || !data.online || daily.loading}
              select={(command, params) => void run(command, params)}
            />
          </div>
          <div hidden={method !== "minute"}>
            {!versions.length ? (
              <p className="subtle">{t("历史仓库还没有分钟线。请先在数据页下载分钟线。")}</p>
            ) : (
              <form
                aria-label={t("分钟线合约")}
                onSubmit={event => {
                  event.preventDefault();
                  if (!daily.held) return;
                  void run("data.dataset.select", {
                    source_dataset_ids: [source, ...extraSources].sort(),
                    daily: daily.held.table,
                    begin_day,
                    end_day,
                    price_increment,
                    multiplier,
                  });
                }}
              >
                <fieldset disabled={busy || !data.online || loading}>
                  <div className="futures-fields">
                    <label className="dataset-version-field">
                      {t("K 线来源")}
                      <select
                        aria-label={t("K 线来源")}
                        value={source}
                        onChange={event => choose(event.target.value)}
                        required
                      >
                        <option value="">{t("选择历史数据版本")}</option>
                        {versions.map(item => (
                          <option key={item.id} value={item.id}>
                            {label(item)}
                          </option>
                        ))}
                      </select>
                    </label>
                    <label>
                      {t("开始交易日")}
                      <input
                        type="date"
                        value={begin_day}
                        onChange={event => setDraft({ ...draft, begin_day: event.target.value })}
                      />
                    </label>
                    <label>
                      {t("结束交易日")}
                      <input
                        type="date"
                        value={end_day}
                        onChange={event => setDraft({ ...draft, end_day: event.target.value })}
                      />
                    </label>
                    <label>
                      {t("最小变动价位")}
                      <input
                        inputMode="decimal"
                        value={price_increment}
                        onChange={event =>
                          setDraft({ ...draft, price_increment: event.target.value })
                        }
                        required
                      />
                    </label>
                    <label>
                      {t("合约乘数")}
                      <input
                        inputMode="decimal"
                        value={multiplier}
                        onChange={event => setDraft({ ...draft, multiplier: event.target.value })}
                        required
                      />
                    </label>
                  </div>
                  {chosen && additionalSources.length > 0 && (
                    <details className="dataset-composition">
                      <summary>
                        {t("拼接更多下载")} ·{" "}
                        {t("K 线 {bars} 份", { bars: 1 + extraSources.length })}
                      </summary>
                      <p className="subtle">
                        {t(
                          "选择同合约、同周期的其他下载；相同记录去重，冲突记录拒绝使用。最多 32 份。",
                        )}
                      </p>
                      <fieldset>
                        <legend>{t("补充 K 线")}</legend>
                        {additionalSources.map(item => (
                          <label className="dataset-segment" key={item.id}>
                            <input
                              type="checkbox"
                              checked={extraSources.includes(item.id)}
                              disabled={
                                !extraSources.includes(item.id) && extraSources.length >= 31
                              }
                              onChange={() => toggle(item.id)}
                            />
                            <span>{label(item)}</span>
                          </label>
                        ))}
                      </fieldset>
                    </details>
                  )}
                  {chosen && settled && daily.held && (
                    <p className="subtle">
                      {t(
                        "结算价和交易日取自全市场日线表：{exchange} 已入库 {first} — {last}，按快照 {snapshot} 读取，之后入库的数据不影响这次选择。",
                        {
                          exchange: settled.exchange,
                          first: settled.first_period,
                          last: settled.last_period,
                          snapshot: daily.held.table.snapshot.slice(0, 12),
                        },
                      )}
                    </p>
                  )}
                  {chosen && settled && <DailyTableGaps stored={settled} />}
                  {chosen && !daily.loading && !settled && (
                    <p role="status">
                      {t(
                        "全市场日线表里还没有 {exchange} 的数据，结算价无处可取。请先在「数据 → 全市场数据」更新日线，范围要覆盖这些 K 线的交易日。",
                        { exchange: exchange ?? "" },
                      )}
                    </p>
                  )}
                  <div className="source-actions">
                    <button
                      type="submit"
                      className="primary"
                      disabled={!chosen || !settled || (!replacing && members >= maxMembers)}
                    >
                      {replacing ? t("更新此合约") : all.length ? t("加入组合") : t("使用此数据集")}
                    </button>
                    <span className="subtle">
                      {t(
                        "交易日区间留空表示全部；组合内合约须覆盖相同交易日；合约单位以交易所公布为准。",
                      )}
                    </span>
                  </div>
                </fieldset>
              </form>
            )}
          </div>
          <div hidden={method !== "minute-series"}>
            <DominantSeriesPicker
              key={data.connection_id}
              snapshot={snapshot}
              versions={versions}
              disabled={busy || !data.online || loading}
              daily={daily.held}
              select={params => void run("data.dataset.series", params)}
            />
          </div>
        </>
      )}
      <div hidden={method !== "saved"}>
        <SavedDatasets
          key={data?.connection_id}
          snapshot={snapshot}
          busy={busy}
          query={query}
          revision={savedRevision}
          trade={trade}
          onSelected={onSelected}
        />
      </div>
      {onDownload && (
        <div className="source-actions">
          <button type="button" onClick={onDownload}>
            {t("前往下载数据")}
          </button>
        </div>
      )}
      {error && (
        <p role="alert" className="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
