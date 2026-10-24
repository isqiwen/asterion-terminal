import { useCallback, useEffect, useRef, useState } from "react";
import {
  type TerminalContext,
  translate,
  type MessageValues,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
import type { HistoryDatasetRecord, HistoryUsage, HistoryReference } from "../../src/bridge/client";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const kinds = {
  download: "来源下载",
  backtest: "回测任务",
  factor: "因子任务",
  sample: "独立研究样本",
  saved_dataset: "已保存数据集",
};
const roles = { market: "行情输入", output: "下载产物" };
export function HistoryUsagePanel({
  context,
  item,
  onClose,
}: {
  context: TerminalContext;
  item: HistoryDatasetRecord;
  onClose: () => void;
}) {
  const [usage, setUsage] = useState<HistoryUsage | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const query = useRef(context.query);
  query.current = context.query;
  const sequence = useRef({ value: 0 });
  const panel = useRef<HTMLElement>(null);
  const load = useCallback(async () => {
    const current = ++sequence.current.value;
    setUsage(null);
    setError("");
    setLoading(true);
    try {
      const response = await query.current("data.history.usage", { id: item.id });
      if (current === sequence.current.value) setUsage(response.history_usage ?? null);
    } catch (e) {
      if (current === sequence.current.value) setError(asDisplayError(e));
    } finally {
      if (current === sequence.current.value) setLoading(false);
    }
  }, [item.id]);
  useEffect(() => {
    const generation = sequence.current;
    panel.current?.scrollIntoView({ block: "nearest" });
    void load();
    return () => {
      ++generation.value;
    };
  }, [load]);
  return (
    <section ref={panel} className="history-usage" aria-label={t("使用情况")} aria-busy={loading}>
      <div className="source-actions">
        <h3>{t("使用情况")}</h3>
        <button type="button" onClick={onClose}>
          {t("关闭")}
        </button>
      </div>
      <p>
        {item.contract_id} · {`${item.interval_minutes}m`} · {item.source}
      </p>
      <p className="subtle">
        {item.begin} — {item.end}
      </p>
      <details className="history-usage-scope">
        <summary>{t("内容版本")}</summary>
        <code>{item.revision}</code>
      </details>
      <p>{t("由数据拥有者核对引用与本窗口草稿；删除请使用版本维护。")}</p>
      {loading && <p role="status">{t("正在核对使用情况…")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {usage && (
        <>
          <h4>{t("当前数据服务")}</h4>
          <p role="status">
            {usage.references.length + usage.consumers.length
              ? t("找到 {count} 条关联记录，请保留此版本。", {
                  count: usage.references.length + usage.consumers.length,
                })
              : t("当前拥有者未登记保留引用；删除仍须重新预览和确认。")}
          </p>
          {!!usage.selected_roles.length && (
            <p>
              {t("本窗口回测与因子草稿：{roles}", {
                roles: usage.selected_roles.map(role => t(roles[role])).join("、"),
              })}
            </p>
          )}
          <ReferenceTable rows={usage.references} />
          {!!usage.consumers.length && (
            <div className="history-usage-table">
              <h4>{t("跨域保留引用")}</h4>
              <p>{t("准备和正式引用均受保护；消费方离线不会解除引用。")}</p>
              <table className="coverage-table" aria-label={t("跨域保留引用")}>
                <thead>
                  <tr>
                    <th>{t("所属节点与服务")}</th>
                    <th>{t("引用对象")}</th>
                    <th>{t("保留状态")}</th>
                  </tr>
                </thead>
                <tbody>
                  {usage.consumers.map(consumer => (
                    <tr
                      key={`${consumer.node}:${consumer.service}:${consumer.id}:${consumer.version}`}
                    >
                      <td>
                        {consumer.node} / {consumer.service}
                      </td>
                      <td>
                        {consumer.kind} / {consumer.id}
                        <details>
                          <summary>{t("固定输入身份")}</summary>
                          <code>{consumer.version}</code>
                        </details>
                      </td>
                      <td>
                        {t(consumer.phase === "prepared" ? "准备中的引用仍保留" : "正式引用仍保留")}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </>
      )}
      <details className="history-usage-scope">
        <summary>{t("检查范围与保留说明")}</summary>
        <p>{t("包含数据拥有者的采集、已保存数据集和跨域保留；任务结束或取消不自动解除引用。")}</p>
        <p>
          {t(
            "不通过进程清单推断引用；引用账本不可访问时明确失败。本窗口草稿另列，删除执行仍由数据拥有者裁决。",
          )}
        </p>
      </details>
      <button type="button" disabled={loading || context.busy} onClick={() => void load()}>
        {t("刷新使用情况")}
      </button>
    </section>
  );
}

function ReferenceTable({ rows }: { rows: HistoryReference[] }) {
  if (!rows.length) return null;
  return (
    <div className="history-usage-table">
      <table className="coverage-table" aria-label={t("数据版本关联记录")}>
        <thead>
          <tr>
            <th>{t("记录类型")}</th>
            <th>{t("关联记录")}</th>
            <th>{t("用途")}</th>
          </tr>
        </thead>
        <tbody>
          {rows.map(row => (
            <tr key={`${row.kind}:${row.id}`}>
              <td>{t(kinds[row.kind])}</td>
              <td>
                {row.name || row.id}
                {row.name && (
                  <details>
                    <summary>{t("记录标识")}</summary>
                    <code>{row.id}</code>
                  </details>
                )}
              </td>
              <td>{row.roles.map(role => t(roles[role])).join("、")}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
