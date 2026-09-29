import { useState } from "react";
import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import { timestamp, type TerminalCommand } from "@asterion/desktop-bridge/client";
import {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  translate,
  getLocale,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
const states = {
  queued: "排队中",
  running: "发布中",
  cancel_requested: "正在取消",
  succeeded: "已发布",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};
export function CalendarPublications({
  snapshot,
  busy,
  trade,
}: Pick<TerminalContext, "snapshot" | "busy" | "trade">) {
  const research = snapshot?.research;
  const [path, setPath] = useWorkspaceDraft("calendarPath", "");
  const [pending, setPending] = useWorkspaceRequestId(
    "calendarPublication",
    JSON.stringify([research?.connection_id, snapshot?.dataset, path]),
  );
  const [error, setError] = useState<DisplayError>("");
  const tasks = research?.tasks.filter(task => task.kind === "calendar_import") ?? [];
  const evidence =
    snapshot?.research_result?.kind === "calendar_import" ? snapshot.research_result : null;
  async function run(method: TerminalCommand, params: Record<string, unknown>) {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  }
  function changePath(value: string) {
    setPath(value);
    setPending(null);
  }
  async function choose() {
    try {
      const selected = await open({
        defaultPath: path || undefined,
        multiple: false,
        directory: false,
        filters: [{ name: "CSV", extensions: ["csv"] }],
      });
      if (typeof selected === "string") changePath(selected);
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  async function publish() {
    const id = pending ?? `calendar-${crypto.randomUUID()}`;
    setPending(id);
    if (await run("research.calendar.submit", { id, path })) setPending(null);
  }
  return (
    <section className="data-publications" aria-label={t("交易日与结算表")}>
      <h3>{t("交易日与结算表")}</h3>
      <p className="dashboard-caption">
        {snapshot?.dataset
          ? `${snapshot.dataset.venue} · ${snapshot.dataset.symbol}`
          : t("先选择合约历史数据")}
      </p>
      <form
        onSubmit={event => {
          event.preventDefault();
          void publish();
        }}
      >
        <fieldset disabled={busy}>
          <div className="futures-file">
            <label>
              {t("结算表 CSV")}
              <input
                aria-label={t("结算表 CSV 路径")}
                required
                value={path}
                onChange={event => changePath(event.target.value)}
              />
            </label>
            {nativeDesktop && (
              <button type="button" onClick={() => void choose()}>
                {t("选择文件")}
              </button>
            )}
          </div>
          <button type="submit" disabled={!research?.online || !snapshot?.dataset}>
            {t(pending ? "确认发布状态" : "发布结算表")}
          </button>
        </fieldset>
      </form>
      <details className="futures-help">
        <summary>{t("文件格式与校验规则")}</summary>
        <code>
          trading_day,session_begin,session_end,settlement_price,schedule_source,settlement_source
        </code>
        <p>
          {t("每行一个时段，时间必须带时区；最多 64 个交易日 / 1 MiB。不推断节假日或补全结算价。")}
        </p>
      </details>
      {error && (
        <p role="alert" className="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.data-workbench" />
        </p>
      )}
      {!tasks.length ? (
        <p className="dashboard-caption">{t("暂无发布任务")}</p>
      ) : (
        <div className="publication-list">
          {[...tasks].reverse().map(task => (
            <article className="publication-row" key={task.id}>
              <div className="publication-name">
                <strong>{task.source_name}</strong>
                <small>{task.instrument}</small>
                <time dateTime={new Date(task.submitted_at_ms).toISOString()}>
                  {new Date(task.submitted_at_ms).toLocaleString(getLocale())}
                </time>
              </div>
              <span>
                {t(states[task.state])}
                {!research?.online && ` · ${t("最后确认状态")}`}
              </span>
              <div>
                {task.state === "succeeded" && (
                  <button
                    disabled={busy || !research?.online}
                    onClick={() => void run("research.result", { id: task.id })}
                  >
                    {t("查看结算表")}
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
                {["failed", "cancelled", "interrupted"].includes(task.state) && (
                  <button
                    disabled={busy || !research?.online}
                    onClick={() => void run("research.action", { id: task.id, action: "retry" })}
                  >
                    {t("重新运行")}
                  </button>
                )}
              </div>
              <details>
                <summary>{t("详情")}</summary>
                <code>{task.id}</code>
                <p>
                  {task.completed} / {task.total} {t("字节")}
                </p>
                {task.error && <p>{task.error}</p>}
              </details>
            </article>
          ))}
        </div>
      )}
      {evidence && (
        <section aria-label={t("结算表内容")}>
          <h4>
            {evidence.result.source_name} · {evidence.result.calendar.contract.symbol}
          </h4>
          <div className="calendar-table">
            <table>
              <thead>
                <tr>
                  {["交易日", "结算价", "交易时段（北京时间）"].map(label => (
                    <th key={label}>{t(label)}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {evidence.result.calendar.days.map(day => (
                  <tr key={day.trading_day}>
                    <td>{day.trading_day}</td>
                    <td>{day.settlement_price}</td>
                    <td>
                      {day.sessions.map(session => (
                        <div key={session.begin_ns}>
                          {timestamp(session.begin_ns)} – {timestamp(session.end_ns)}
                        </div>
                      ))}
                      <details>
                        <summary>{t("来源说明")}</summary>
                        <p>{day.schedule_source}</p>
                        <p>{day.settlement_source}</p>
                      </details>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <details>
            <summary>{t("版本详情")}</summary>
            <p>
              {t("发布标识")} <code>{evidence.result.id}</code>
            </p>
            <p>
              {t("内容版本")} <code>{evidence.result.calendar.revision}</code>
            </p>
          </details>
        </section>
      )}
    </section>
  );
}
