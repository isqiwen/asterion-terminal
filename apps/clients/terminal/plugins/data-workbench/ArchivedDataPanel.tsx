import "./data.css";
import { HistoryUsagePanel } from "./HistoryUsagePanel";
import { HistoryUpdatePanel } from "./HistoryUpdatePanel";
import { useState, useEffect, useRef, useCallback } from "react";
import {
  type TerminalContext,
  translate,
  type MessageValues,
  ErrorNotice,
  asDisplayError,
  useDailyTable,
  type DisplayError,
} from "../contract";
import type { DailyTable, HistoryCoverage, HistoryDatasetRecord } from "../../src/bridge/client";
import { MinuteRangePanel } from "./MinuteRangePanel";
import { HistoryDatasetViewer } from "./HistoryDatasetViewer";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
export function ArchivedDataPanel(context: TerminalContext) {
  const [filter, setFilter] = useState({ venue: "", product: "", contract_id: "", source: "" });
  const [items, setItems] = useState<HistoryDatasetRecord[]>([]);
  const [selected, setSelected] = useState<HistoryDatasetRecord | null>(null);
  const [update, setUpdate] = useState<{ item: HistoryDatasetRecord; daily?: DailyTable } | null>(
    null,
  );
  // Minute versions are checked against the market-wide daily table.
  const table = useDailyTable(context.snapshot, context.query);
  const held = table.held?.table;
  const [usage, setUsage] = useState<HistoryDatasetRecord | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const [coverage, setCoverage] = useState<HistoryCoverage[] | null>(null);
  // The opened detail is brought into view once; later redraws leave the
  // scroll position to the user.
  const detail = useRef<HTMLDivElement>(null);
  const opened = usage?.id ?? update?.item.id ?? selected?.id;
  useEffect(() => {
    detail.current?.scrollIntoView({ block: "nearest" });
  }, [opened, update?.daily]);
  const query = useRef(context.query);
  query.current = context.query;
  const online = !!context.snapshot?.data?.online;
  const connection = context.snapshot?.data?.connection_id;
  const sequence = useRef(0);
  const load = useCallback(async (values: typeof filter) => {
    const current = ++sequence.current;
    setSelected(null);
    setUpdate(null);
    setUsage(null);
    setCoverage(null);
    setLoading(true);
    setError("");
    try {
      const response = await query.current("data.datasets", values);
      if (current === sequence.current) {
        setItems(response.history_datasets ?? []);
      }
    } catch (e) {
      if (current === sequence.current) setError(asDisplayError(e));
    } finally {
      if (current === sequence.current) setLoading(false);
    }
  }, []);
  const check = useCallback(async (values: typeof filter, daily: DailyTable) => {
    const current = ++sequence.current;
    setLoading(true);
    setError("");
    try {
      const [response, archives] = await Promise.all([
        query.current("data.coverage", { ...values, daily }),
        query.current("data.datasets", values),
      ]);
      if (current === sequence.current) {
        setItems(archives.history_datasets ?? []);
        setCoverage(response.history_coverage ?? []);
        setSelected(null);
        setUpdate(null);
        setUsage(null);
      }
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
    setUpdate(null);
    setUsage(null);
    if (online) void load({ venue: "", product: "", contract_id: "", source: "" });
    return cancel;
  }, [online, connection, load, cancel]);
  return (
    <section className="futures-data data-archive" aria-label={t("历史数据仓库")}>
      <h2>{t("历史数据仓库")}</h2>
      {!online && (
        <button
          disabled={context.busy}
          onClick={() => void context.trade("node.data_tasks.local.open")}
        >
          {t("连接数据服务")}
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
          disabled={!online || loading || context.busy || !filter.product || !held}
          title={
            !filter.product
              ? t("按品种核对覆盖，请先填写品种")
              : held
                ? undefined
                : t("覆盖核对按全市场日线表进行，请先在全市场数据里更新日线")
          }
          onClick={() => held && void check(filter, held)}
        >
          {t("覆盖核对")}
        </button>
      </form>
      {coverage && (
        <section aria-label={t("覆盖核对")}>
          <h3>{t("覆盖核对")}</h3>
          <p className="subtle">
            {t(
              "仅在这个版本已标记的首末交易日之间，与全市场日线表核对整日缺口。首尾交易日覆盖与日内完整性尚未验证；有记录缺少交易日标记时，无法核对整日缺口。",
            )}
          </p>
          {!coverage.length ? (
            <p>{t("没有符合条件的历史数据")}</p>
          ) : (
            <table className="coverage-table" aria-label={t("合约覆盖")}>
              <thead>
                <tr>
                  <th>{t("合约身份")}</th>
                  <th>{t("数据版本")}</th>
                  <th>{t("分钟线交易日")}</th>
                  <th>{t("日线表中的成交日")}</th>
                  <th>{t("缺失交易日")}</th>
                  <th>{t("操作")}</th>
                </tr>
              </thead>
              <tbody>
                {coverage.map(row => (
                  <tr key={row.minute_dataset_id}>
                    <td>{row.contract_id}</td>
                    <td>
                      {row.interval_minutes}m · {row.minute_source} ·{" "}
                      <code>{row.minute_dataset_id.slice(0, 8)}</code>
                    </td>
                    <td>
                      {row.minute_days
                        ? `${row.minute_days} · ${row.minute_first} — ${row.minute_last}`
                        : "—"}
                    </td>
                    <td>
                      {row.compared_first
                        ? `${row.traded_days} · ${row.compared_first} — ${row.compared_last}`
                        : row.unknown_rows
                          ? t("有 {n} 条记录缺少交易日标记，无法核对整日缺口", {
                              n: row.unknown_rows,
                            })
                          : t("表中没有这段时间的日线")}
                    </td>
                    <td>
                      {row.compared_first
                        ? row.uncovered
                          ? t("{n} 个：{days}", {
                              n: row.uncovered,
                              days:
                                row.uncovered_days.slice(0, 5).join("、") +
                                (row.uncovered > 5 ? " …" : ""),
                            })
                          : t("无")
                        : "—"}
                    </td>
                    <td>
                      {row.uncovered > 0 && (
                        <button
                          disabled={
                            context.busy ||
                            loading ||
                            !online ||
                            context.snapshot?.task_service?.remote
                          }
                          onClick={() => {
                            const item = items.find(item => item.id === row.minute_dataset_id);
                            if (item && held) {
                              setUpdate({ item, daily: held });
                              setUsage(null);
                              setSelected(null);
                            }
                          }}
                        >
                          {t("补齐缺口")}
                        </button>
                      )}
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
          <article key={item.id} data-dataset-id={item.id} className="publication-row archive-row">
            <div>
              <strong>{item.contract_id}</strong>
              <p>
                {item.source} · {`${item.interval_minutes}m`}
              </p>
              <p>
                {item.begin} — {item.end} · {item.rows} {t("记录数")}
              </p>
            </div>
            <div className="archive-actions">
              <button
                aria-pressed={usage?.id === item.id}
                disabled={!online || context.busy}
                onClick={() => {
                  setUsage(item);
                  setUpdate(null);
                  setSelected(null);
                }}
              >
                {t("使用情况")}
              </button>
              <button
                aria-pressed={selected?.id === item.id}
                disabled={!online || context.busy}
                onClick={() => {
                  setSelected(item);
                  setUsage(null);
                  setUpdate(null);
                }}
              >
                {t("查看数据")}
              </button>
              <button
                aria-pressed={update?.item.id === item.id}
                disabled={!online || context.busy || context.snapshot?.task_service?.remote}
                onClick={() => {
                  setUpdate({ item });
                  setUsage(null);
                  setSelected(null);
                }}
              >
                {t("下载后续数据")}
              </button>
              {/* Daily bars of a backtest are the market-wide daily table's. */}
              {item.interval_minutes > 0 && (
                <button
                  disabled={!online || context.busy}
                  onClick={() =>
                    context.navigate("workspace.research", {
                      page: "backtest",
                      params: {
                        source_dataset_id: item.id,
                        connection_id: connection ?? "",
                        selection_id: crypto.randomUUID(),
                      },
                    })
                  }
                >
                  {t("用于回测")}
                </button>
              )}
            </div>
            <details>
              <summary>{t("内容版本")}</summary>
              <code>{item.revision}</code>
            </details>
            {/* What was asked of this version opens under its own row. */}
            {(usage?.id === item.id || update?.item.id === item.id || selected?.id === item.id) && (
              <div className="archive-detail" ref={detail}>
                {usage?.id === item.id && (
                  <HistoryUsagePanel
                    key={`${connection}:${usage.id}`}
                    context={context}
                    item={usage}
                    onClose={() => setUsage(null)}
                  />
                )}
                {update?.item.id === item.id && (
                  <HistoryUpdatePanel
                    key={`${connection}:${update.item.id}:${update.daily?.snapshot ?? ""}`}
                    context={context}
                    item={update.item}
                    daily={update.daily}
                    onClose={() => setUpdate(null)}
                  />
                )}
                {selected?.id === item.id && (
                  <>
                    <MinuteRangePanel
                      key={`${connection}:${selected.id}:${JSON.stringify(held)}`}
                      id={selected.id}
                      daily={held ?? null}
                      query={context.query}
                      disabled={!online || context.busy}
                    />
                    <HistoryDatasetViewer
                      {...context}
                      id={selected.id}
                      source={selected.source}
                      sourceLabel={selected.source}
                      onClose={() => setSelected(null)}
                    />
                  </>
                )}
              </div>
            )}
          </article>
        ))}
      </div>
    </section>
  );
}
