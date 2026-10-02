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
  bar_factor: "K 线因子任务",
  daily_factor: "日线因子任务",
  saved_dataset: "已保存数据集",
};
const roles = { market: "行情输入", settlement: "结算输入", output: "下载产物" };
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
      const response = await query.current("research.history.usage", { id: item.id });
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
        {item.contract_id} · {item.interval_minutes ? `${item.interval_minutes}m` : t("日线")} ·{" "}
        {item.source}
      </p>
      <p className="subtle">
        {item.begin} — {item.end}
      </p>
      <details className="history-usage-scope">
        <summary>{t("内容版本")}</summary>
        <code>{item.revision}</code>
      </details>
      <p>{t("核对研究记录和本窗口草稿。尚未开放历史版本删除。")}</p>
      {loading && <p role="status">{t("正在核对使用情况…")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {usage && (
        <>
          <h4>{t("当前研究服务")}</h4>
          <p role="status">
            {usage.references.length
              ? t("找到 {count} 条关联记录，请保留此版本。", { count: usage.references.length })
              : t("当前研究服务中未发现关联记录；这不代表该版本可以删除。")}
          </p>
          {!!usage.selected_roles.length && (
            <p>
              {t("本窗口研究草稿：{roles}", {
                roles: usage.selected_roles.map(role => t(roles[role])).join("、"),
              })}
            </p>
          )}
          <ReferenceTable rows={usage.references} />
          {!!usage.other_research.length && (
            <section aria-label={t("其他研究服务")}>
              <h4>{t("其他研究服务")}</h4>
              <p className="subtle">
                {t("检查已连接节点的任务与已保存数据集；停止的本机服务只读检查账本。")}
              </p>
              {usage.other_research.map(group => (
                <section
                  key={`${group.node}:${group.service}`}
                  aria-label={`${group.node === "local" ? t("本机") : group.node} / ${group.service || t("服务清单")}`}
                >
                  <h5>
                    {group.node === "local" ? t("本机") : group.node} /{" "}
                    {group.service || t("服务清单")}
                  </h5>
                  {group.checked ? (
                    <p>
                      {t("已检查，发现 {count} 条关联记录。", { count: group.references.length })}
                    </p>
                  ) : (
                    <div role="alert">
                      <p>{t("研究服务检查未完成，不能视为无引用。")}</p>
                      {group.error && (
                        <details>
                          <summary>{t("未完成检查的详情")}</summary>
                          <ErrorNotice error={group.error} namespace="diagnostics" />
                        </details>
                      )}
                    </div>
                  )}
                  {group.stopped && <p className="subtle">{t("本机账本已检查；服务保持停止。")}</p>}
                  <ReferenceTable rows={group.references} />
                </section>
              ))}
            </section>
          )}
          {(usage.disconnected_nodes.names.length > 0 || usage.disconnected_nodes.error) && (
            <section aria-label={t("未连接的节点")}>
              <h4>{t("未连接的节点")}</h4>
              <p>{t("以下已登记节点未连接，本次未检查；不会自动连接或启动服务。")}</p>
              <ul>
                {usage.disconnected_nodes.names.map(name => (
                  <li key={name}>{name}</li>
                ))}
              </ul>
              {usage.disconnected_nodes.error && (
                <div role="alert">
                  <p>{t("节点登记清单未能完整读取，检查范围不完整。")}</p>
                  <details>
                    <summary>{t("未完成检查的详情")}</summary>
                    <ErrorNotice error={usage.disconnected_nodes.error} namespace="diagnostics" />
                  </details>
                </div>
              )}
            </section>
          )}
        </>
      )}
      <details className="history-usage-scope">
        <summary>{t("检查范围与保留说明")}</summary>
        <p>
          {t(
            "包含来源下载、回测、K 线因子、日线因子和已保存数据集；任务结束或取消后仍保留来源关系。",
          )}
        </p>
        <p>
          {t(
            "未检查停止的远程研究服务、未连接的节点及其他窗口的研究草稿。结果是本次查询的观察值，不是删除许可；检查失败时不能按无引用处理。",
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
