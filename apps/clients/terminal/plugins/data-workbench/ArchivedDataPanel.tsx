import "./data.css";
import { CalendarPublications } from "./CalendarPublications";
import { type TerminalContext, getLocale } from "../contract";
import { ErrorNotice, asDisplayError, type DisplayError } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
import { useState } from "react";
import { timestamp, type TerminalCommand } from "@asterion/desktop-bridge/client";
export function ArchivedDataPanel({ snapshot, busy, navigate, trade }: TerminalContext) {
  const data = snapshot?.dataset ?? null;
  const research = snapshot?.research;
  const tasks = research?.tasks.filter(task => task.kind === "data_import") ?? [];
  const openMarket = () => navigate("workspace.market", { marketMode: "history" });
  async function run(method: TerminalCommand, params: Record<string, unknown> = {}) {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  }
  const [error, setError] = useState<DisplayError>("");
  const [message, setMessage] = useState("");
  return (
    <section className="futures-data" aria-label={t("数据存档与结算表")}>
      <div className="panel-heading">
        <h2>{t("数据存档与结算表")}</h2>
        <span className="panel-spacer" />
        <small>{t("已有数据存档与结算表")}</small>
      </div>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.data-workbench" />
        </p>
      )}
      {message && (
        <p role="status" className="notice">
          {t(message)}
        </p>
      )}
      {data && (
        <div className="dataset-summary">
          <div>
            <dt>{t(data.persistent ? "已发布数据" : "当前预览")}</dt>
            <dd>
              {data.venue} · {data.symbol}
            </dd>
          </div>
          <div>
            <dt>{t("记录数")}</dt>
            <dd>
              {data.count.toLocaleString(getLocale())} {t("笔")}
            </dd>
          </div>
          <div>
            <dt>{t("数据截至（北京时间）")}</dt>
            <dd>{timestamp(data.last_timestamp_ns)}</dd>
          </div>
        </div>
      )}
      {data && (
        <div className="source-actions">
          <button onClick={openMarket}>{t("查看行情")}</button>
          {data.persistent && (
            <button onClick={() => navigate("workspace.research")}>{t("开始研究")}</button>
          )}
        </div>
      )}
      {data?.publication_id && (
        <details className="futures-help">
          <summary>{t("版本详情")}</summary>
          <p>
            {t("发布标识")} <code>{data.publication_id}</code>
          </p>
          <p>
            {t("内容版本")} <code>{data.revision}</code>
          </p>
        </details>
      )}
      <section className="data-publications" aria-label={t("数据发布")}>
        <div className="panel-heading">
          <h3>{t("数据发布")}</h3>
          <span className="panel-spacer" />
          <small>{research?.remote ? `${research.host} · ${research.service}` : t("本机")}</small>
          {!research?.online && (
            <button disabled={busy} onClick={() => void run("research.local")}>
              {t("连接研究服务")}
            </button>
          )}
        </div>
        {!tasks.length ? (
          <p className="dashboard-caption">{t("暂无发布任务")}</p>
        ) : (
          <div className="publication-list">
            {[...tasks].reverse().map(task => (
              <article key={task.id} className="publication-row">
                <div className="publication-name">
                  <strong>{task.source_name}</strong>
                  <small>{task.instrument}</small>
                  <time
                    aria-label={t("提交时间")}
                    dateTime={new Date(task.submitted_at_ms).toISOString()}
                  >
                    {new Date(task.submitted_at_ms).toLocaleString(getLocale())}
                  </time>
                </div>
                <span>
                  {t(
                    {
                      queued: "排队中",
                      running: "发布中",
                      cancel_requested: "正在取消",
                      succeeded: "已发布",
                      failed: "失败",
                      cancelled: "已取消",
                      interrupted: "已中断",
                    }[task.state],
                  )}
                  {!research?.online && ` · ${t("最后确认状态")}`}
                </span>
                <div>
                  {task.state === "succeeded" && (
                    <button
                      disabled={busy || !research?.online}
                      onClick={() =>
                        void run("research.data.use", { id: task.id }).then(ok => {
                          if (ok) {
                            setMessage("已载入发布版本，可查看行情或开始研究。");
                          }
                        })
                      }
                    >
                      {t("使用此版本")}
                    </button>
                  )}
                  {task.state === "succeeded" && (
                    <button
                      disabled={busy || !research?.online}
                      onClick={() =>
                        void run("research.data.use", { id: task.id }).then(ok => {
                          if (ok) {
                            navigate("workspace.research");
                          }
                        })
                      }
                    >
                      {t("使用并开始研究")}
                    </button>
                  )}
                  {["queued", "running"].includes(task.state) && (
                    <button
                      disabled={busy || !research?.online}
                      onClick={() => void run("research.action", { id: task.id, action: "cancel" })}
                    >
                      {t("取消")}
                    </button>
                  )}
                </div>
                <details>
                  <summary>{t("详情")}</summary>
                  <code>{task.id}</code>
                  <p>
                    {task.completed} / {task.total} {t("字节")}
                  </p>
                  <p>
                    {t("最近更新")} · {new Date(task.updated_at_ms).toLocaleString(getLocale())}
                  </p>
                  {task.error && <p>{task.error}</p>}
                </details>
              </article>
            ))}
          </div>
        )}
      </section>
      <CalendarPublications snapshot={snapshot} busy={busy} trade={trade} />
    </section>
  );
}
