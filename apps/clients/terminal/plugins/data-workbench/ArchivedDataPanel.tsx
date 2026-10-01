import "./data.css";
import { useState, useEffect, useRef, useCallback } from "react";
import {
  type TerminalContext,
  translate,
  type MessageValues,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
import type { HistoryCoverage, HistoryDatasetRecord } from "../../src/bridge/client";
import { HistoryDatasetViewer } from "./HistoryDatasetViewer";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
export function ArchivedDataPanel(context: TerminalContext) {
  const [filter, setFilter] = useState({ venue: "", product: "", contract_id: "", source: "" });
  const [items, setItems] = useState<HistoryDatasetRecord[]>([]);
  const [selected, setSelected] = useState<HistoryDatasetRecord | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const [coverage, setCoverage] = useState<HistoryCoverage[] | null>(null);
  const query = useRef(context.query);
  query.current = context.query;
  const online = !!context.snapshot?.research?.online;
  const connection = context.snapshot?.research?.connection_id;
  const sequence = useRef(0);
  const load = useCallback(async (values: typeof filter) => {
    const current = ++sequence.current;
    setSelected(null);
    setLoading(true);
    setError("");
    try {
      const response = await query.current("research.datasets", values);
      if (current === sequence.current) setItems(response.history_datasets ?? []);
    } catch (e) {
      if (current === sequence.current) setError(asDisplayError(e));
    } finally {
      if (current === sequence.current) setLoading(false);
    }
  }, []);
  const check = useCallback(async (values: typeof filter) => {
    const current = ++sequence.current;
    setLoading(true);
    setError("");
    try {
      const response = await query.current("research.coverage", values);
      if (current === sequence.current) setCoverage(response.history_coverage ?? []);
    } catch (e) {
      if (current === sequence.current) setError(asDisplayError(e));
    } finally {
      if (current === sequence.current) setLoading(false);
    }
  }, []);
  const cancel = useCallback(() => {
    ++sequence.current;
  }, []);
  useEffect(() => {
    setFilter({ venue: "", product: "", contract_id: "", source: "" });
    setItems([]);
    setCoverage(null);
    setSelected(null);
    if (online) void load({ venue: "", product: "", contract_id: "", source: "" });
    return cancel;
  }, [online, connection, load, cancel]);
  return (
    <section className="futures-data" aria-label={t("历史数据仓库")}>
      <h2>{t("历史数据仓库")}</h2>
      {!online && (
        <button disabled={context.busy} onClick={() => void context.trade("research.local")}>
          {t("连接研究服务")}
        </button>
      )}
      <form
        className="source-actions"
        onSubmit={event => {
          event.preventDefault();
          void load(filter);
        }}
      >
        {(["venue", "product", "contract_id", "source"] as const).map((key, index) => (
          <label key={key}>
            {t(["交易所", "品种", "合约身份", "数据源"][index])}
            <input
              value={filter[key]}
              onChange={event => setFilter({ ...filter, [key]: event.target.value })}
            />
          </label>
        ))}
        <button disabled={!online || loading || context.busy}>{t("查询")}</button>
        <button
          type="button"
          disabled={!online || loading || context.busy || !filter.product}
          title={filter.product ? undefined : t("按品种核对覆盖，请先填写品种")}
          onClick={() => void check(filter)}
        >
          {t("覆盖核对")}
        </button>
      </form>
      {coverage && (
        <section aria-label={t("覆盖核对")}>
          <h3>{t("覆盖核对")}</h3>
          <p className="subtle">
            {t(
              "按合约统计仓库中分钟线与日线覆盖的交易日；缺失交易日是分钟线起止范围内、日线有而分钟线没有的日期。所有文件先校验摘要再统计。",
            )}
          </p>
          {!coverage.length ? (
            <p>{t("没有符合条件的历史数据")}</p>
          ) : (
            <table className="coverage-table" aria-label={t("合约覆盖")}>
              <thead>
                <tr>
                  <th>{t("合约身份")}</th>
                  <th>{t("分钟线交易日")}</th>
                  <th>{t("日线交易日")}</th>
                  <th>{t("缺失交易日")}</th>
                </tr>
              </thead>
              <tbody>
                {coverage.map(row => (
                  <tr key={row.contract_id}>
                    <td>{row.contract_id}</td>
                    <td>
                      {row.minute_days
                        ? `${row.minute_days} · ${row.minute_first} — ${row.minute_last}`
                        : "—"}
                    </td>
                    <td>
                      {row.daily_days
                        ? `${row.daily_days} · ${row.daily_first} — ${row.daily_last}`
                        : "—"}
                    </td>
                    <td>
                      {row.minute_days && row.daily_days
                        ? row.uncovered
                          ? t("{n} 个：{days}", {
                              n: row.uncovered,
                              days:
                                row.uncovered_days.slice(0, 5).join("、") +
                                (row.uncovered > 5 ? " …" : ""),
                            })
                          : t("无")
                        : t("需要分钟线与日线")}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </section>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.data-workbench" />
        </p>
      )}
      <p className="subtle">{t("历史仓库说明")}</p>
      {!items.length && <p>{t("暂无历史数据版本")}</p>}
      <div className="publication-list">
        {items.map(item => (
          <article key={item.id} className="publication-row">
            <div>
              <strong>{item.contract_id}</strong>
              <p>
                {item.source} · {item.interval_minutes ? `${item.interval_minutes}m` : t("日线")}
              </p>
              <p>
                {item.begin} — {item.end} · {item.rows} {t("记录数")}
              </p>
            </div>
            <button disabled={!online || context.busy} onClick={() => setSelected(item)}>
              {t("查看数据")}
            </button>
            <details>
              <summary>{t("内容版本")}</summary>
              <code>{item.revision}</code>
            </details>
          </article>
        ))}
      </div>
      {selected && (
        <HistoryDatasetViewer
          {...context}
          id={selected.id}
          archive
          source={selected.source}
          sourceLabel={selected.source}
          timeAxis={selected.interval_minutes ? "instant" : "trading-day"}
          onClose={() => setSelected(null)}
        />
      )}
    </section>
  );
}
