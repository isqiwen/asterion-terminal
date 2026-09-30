import "./data.css";
import { useState, useEffect, useRef, useCallback } from "react";
import {
  type TerminalContext,
  translate,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
import type { HistoryDatasetRecord } from "../../src/bridge/client";
import { HistoryDatasetViewer } from "./HistoryDatasetViewer";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
export function ArchivedDataPanel(context: TerminalContext) {
  const [filter, setFilter] = useState({ venue: "", product: "", contract_id: "", source: "" });
  const [items, setItems] = useState<HistoryDatasetRecord[]>([]);
  const [selected, setSelected] = useState<HistoryDatasetRecord | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
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
  const cancel = useCallback(() => {
    ++sequence.current;
  }, []);
  useEffect(() => {
    setFilter({ venue: "", product: "", contract_id: "", source: "" });
    setItems([]);
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
      </form>
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
