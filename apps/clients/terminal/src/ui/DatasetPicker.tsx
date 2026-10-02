import { SavedDatasets } from "./SavedDatasets";
import { useEffect, useState } from "react";
import { translate, type MessageValues } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { HistoryDatasetRecord, Snapshot, TerminalCommand } from "../bridge/client";
import { useHistoryDatasets, type HistoryQuery } from "./useHistoryDatasets";
import { useWorkspaceDraft } from "../host/workspace/drafts";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

const label = (item: HistoryDatasetRecord) =>
  `${item.contract_id} · ${item.interval_minutes ? t("{n} 分钟", { n: item.interval_minutes }) : t("日线")} · ${item.begin.slice(0, 10)} — ${item.end.slice(0, 10)} · ${item.source} · ${item.id.slice(0, 8)}`;

// Mirrors the core's portfolio limit.
const maxContracts = 20;

// Chooses published archive versions for backtests and factors; a portfolio
// holds one dataset per
// contract and all of them share the same trading days. The contract identity
// comes from the archive; only units a data source does not provide are
// entered here.
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
  query: HistoryQuery;
  locked?: boolean;
  sourceRequest?: { id: string; connection: string; request: string };
  onDownload?: () => void;
  onSelected?: () => void;
}) {
  const selected = snapshot?.datasets ?? [];
  const { items: versions, error: loadError, loading } = useHistoryDatasets(snapshot, query);
  const [draft, setDraft] = useWorkspaceDraft(
    `dataset-picker:composition:${snapshot?.research?.connection_id}`,
    {
      source: "",
      settlement: "",
      extraSources: [] as string[],
      extraSettlements: [] as string[],
      begin_day: "",
      end_day: "",
      price_increment: "",
      multiplier: "",
      consumed: "",
    },
  );
  const {
    source,
    settlement,
    extraSources,
    extraSettlements,
    begin_day,
    end_day,
    price_increment,
    multiplier,
  } = draft;
  const [error, setError] = useState<DisplayError>("");
  const chosen = versions.find(item => item.id === source);
  const replacing = selected.some(
    item =>
      `${item.contract.venue}/${item.contract.product.toLowerCase()}/${item.contract.delivery_month}` ===
      chosen?.contract_id,
  );
  const settlements = versions.filter(
    item => item.interval_minutes === 0 && item.contract_id === chosen?.contract_id,
  );
  const additionalSources = versions.filter(
    item =>
      item.id !== source &&
      item.contract_id === chosen?.contract_id &&
      item.interval_minutes === chosen?.interval_minutes &&
      item.source === chosen?.source,
  );
  const settlementSource = settlements.find(item => item.id === settlement)?.source;
  const additionalSettlements = settlements.filter(
    item => item.id !== settlement && item.source === settlementSource,
  );
  function toggle(role: "extraSources" | "extraSettlements", id: string) {
    setDraft(previous => ({
      ...previous,
      [role]: previous[role].includes(id)
        ? previous[role].filter(value => value !== id)
        : [...previous[role], id],
    }));
  }
  function selection(id: string) {
    const item = versions.find(item => item.id === id);
    const candidates = versions.filter(
      candidate => candidate.interval_minutes === 0 && candidate.contract_id === item?.contract_id,
    );
    const daily =
      item?.interval_minutes === 0 ? item : candidates.length === 1 ? candidates[0] : undefined;
    const listed = snapshot?.market?.catalog.contracts.find(
      contract => contract.contract_id === item?.contract_id,
    );
    // Never reuse another contract's manually entered exchange units or dates.
    return {
      source: item?.id ?? "",
      settlement: daily?.id ?? "",
      extraSources: [],
      extraSettlements: [],
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
      !snapshot?.research?.online ||
      draft.consumed === sourceRequest.request
    )
      return;
    const valid =
      sourceRequest.connection === snapshot.research.connection_id &&
      versions.some(item => item.id === sourceRequest.id);
    setDraft(previous => ({
      ...previous,
      ...selection(valid ? sourceRequest.id : ""),
      consumed: sourceRequest.request,
    }));
    setError(valid ? "" : t("数据版本不属于当前研究服务，请重新选择数据。"));
    // Consume each navigation intent once; subsequent edits belong to this draft.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [
    loading,
    sourceRequest?.request,
    snapshot?.research?.connection_id,
    snapshot?.research?.online,
    draft.consumed,
  ]);
  async function run(method: TerminalCommand, params: Record<string, unknown> = {}) {
    setError("");
    try {
      await trade(method, params);
      if (method === "research.dataset.select") onSelected?.();
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
            {t("K 线 {bars} 份 · 结算 {settlements} 份", {
              bars: item.source_dataset_ids.length,
              settlements: item.settlement_dataset_ids.length,
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
  return (
    <form
      className="dataset-picker"
      aria-label={t("历史数据集")}
      onSubmit={event => {
        event.preventDefault();
        void run("research.dataset.select", {
          source_dataset_ids: [source, ...extraSources].sort(),
          settlement_dataset_ids: [settlement, ...extraSettlements].sort(),
          begin_day,
          end_day,
          price_increment,
          multiplier,
        });
      }}
    >
      <h3>{t("历史数据集")}</h3>
      <SavedDatasets
        key={snapshot?.research?.connection_id}
        snapshot={snapshot}
        busy={busy}
        query={query}
        trade={trade}
        onSelected={onSelected}
      />
      {list}
      {selected.length >= maxContracts && (
        <p className="subtle">{t("组合最多 {n} 个合约。", { n: maxContracts })}</p>
      )}
      {loadError && (
        <p role="alert">
          <ErrorNotice error={loadError} />
        </p>
      )}
      {loading && <p role="status">{t("正在读取历史仓库…")}</p>}
      {!snapshot?.research ? (
        <p className="subtle">{t("研究服务未连接，无法读取已下载的历史数据。")}</p>
      ) : !versions.length ? (
        <p className="subtle">{t("历史仓库还没有数据。请先在数据页下载分钟线或日线。")}</p>
      ) : (
        <fieldset disabled={busy || !snapshot?.research?.online || loading}>
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
            <label
              className="dataset-version-field"
              hidden={!!settlement && settlements.length === 1}
            >
              {t("结算价来源")}
              <select
                aria-label={t("结算价来源")}
                value={settlement}
                onChange={event =>
                  setDraft({ ...draft, settlement: event.target.value, extraSettlements: [] })
                }
                required
              >
                <option value="">
                  {chosen && !settlements.length ? t("需要同一合约的日线数据") : t("选择日线版本")}
                </option>
                {settlements.map(item => (
                  <option key={item.id} value={item.id}>
                    {label(item)}
                  </option>
                ))}
              </select>
            </label>
            <label>
              {t("开始交易日")}
              <input
                aria-label={t("开始交易日")}
                type="date"
                value={begin_day}
                onChange={event => setDraft({ ...draft, begin_day: event.target.value })}
              />
            </label>
            <label>
              {t("结束交易日")}
              <input
                aria-label={t("结束交易日")}
                type="date"
                value={end_day}
                onChange={event => setDraft({ ...draft, end_day: event.target.value })}
              />
            </label>
            <label>
              {t("最小变动价位")}
              <input
                aria-label={t("最小变动价位")}
                inputMode="decimal"
                value={price_increment}
                onChange={event => setDraft({ ...draft, price_increment: event.target.value })}
                required
              />
            </label>
            <label>
              {t("合约乘数")}
              <input
                aria-label={t("合约乘数")}
                inputMode="decimal"
                value={multiplier}
                onChange={event => setDraft({ ...draft, multiplier: event.target.value })}
                required
              />
            </label>
          </div>
          {chosen && (additionalSources.length > 0 || additionalSettlements.length > 0) && (
            <details className="dataset-composition">
              <summary>
                {t("拼接更多下载")} ·{" "}
                {t("K 线 {bars} 份 · 结算 {settlements} 份", {
                  bars: 1 + extraSources.length,
                  settlements: (settlement ? 1 : 0) + extraSettlements.length,
                })}
              </summary>
              <p className="subtle">
                {t(
                  "选择同合约、同周期的其他下载；相同记录去重，冲突记录拒绝使用。每类最多 32 份。",
                )}
              </p>
              {additionalSources.length > 0 && (
                <fieldset>
                  <legend>{t("补充 K 线")}</legend>
                  {additionalSources.map(item => (
                    <label className="dataset-segment" key={item.id}>
                      <input
                        type="checkbox"
                        checked={extraSources.includes(item.id)}
                        disabled={!extraSources.includes(item.id) && extraSources.length >= 31}
                        onChange={() => toggle("extraSources", item.id)}
                      />
                      <span>{label(item)}</span>
                    </label>
                  ))}
                </fieldset>
              )}
              {additionalSettlements.length > 0 && (
                <fieldset>
                  <legend>{t("补充结算价")}</legend>
                  {additionalSettlements.map(item => (
                    <label className="dataset-segment" key={item.id}>
                      <input
                        type="checkbox"
                        checked={extraSettlements.includes(item.id)}
                        disabled={
                          !extraSettlements.includes(item.id) && extraSettlements.length >= 31
                        }
                        onChange={() => toggle("extraSettlements", item.id)}
                      />
                      <span>{label(item)}</span>
                    </label>
                  ))}
                </fieldset>
              )}
            </details>
          )}
          {settlement && settlements.length === 1 && (
            <p className="subtle">{t("已匹配同一合约的日线结算版本。")}</p>
          )}
          {chosen && settlements.length > 1 && !settlement && (
            <p role="status">{t("存在多个日线版本，请明确选择本次使用的结算来源。")}</p>
          )}
          <div className="source-actions">
            <button
              type="submit"
              className="primary"
              disabled={
                !chosen ||
                !settlements.some(item => item.id === settlement) ||
                (!replacing && selected.length >= maxContracts)
              }
            >
              {replacing ? t("更新此合约") : selected.length ? t("加入组合") : t("使用此数据集")}
            </button>
            <span className="subtle">
              {t("交易日区间留空表示全部；组合内合约须覆盖相同交易日；合约单位以交易所公布为准。")}
            </span>
          </div>
        </fieldset>
      )}
      {onDownload && (
        <div className="source-actions">
          {chosen && !settlements.length && (
            <p role="status">{t("此合约缺少日线结算数据，补齐后才能用于回测。")}</p>
          )}
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
    </form>
  );
}
