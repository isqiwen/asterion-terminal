import { useEffect, useRef, useState } from "react";
import {
  type TerminalContext,
  translate,
  type MessageValues,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  getLocale,
} from "../contract";
import {
  type HistoryDatasetRecord,
  type HistoryUpdatePlan,
  savedCredential,
} from "../../src/bridge/client";
import { HistoryDatasetViewer } from "./HistoryDatasetViewer";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
function yesterday() {
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Shanghai",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).format(new Date(Date.now() - 86400000));
}
export function HistoryUpdatePanel({
  context,
  item,
  calendar,
  onClose,
}: {
  context: TerminalContext;
  item: HistoryDatasetRecord;
  calendar?: string;
  onClose: () => void;
}) {
  const panel = useRef<HTMLElement>(null);
  useEffect(() => {
    panel.current?.scrollIntoView({ block: "nearest" });
  }, []);
  const [end, setEnd] = useState(yesterday);
  const source = context.snapshot?.research?.sources.find(source => source.id === item.source);
  const saved = savedCredential(context.snapshot, item.source);
  const [rate, setRate] = useState(String(source?.connection?.requests_per_minute_default ?? 60));
  const [token, setToken] = useState("");
  const [plan, setPlan] = useState<HistoryUpdatePlan | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [working, setWorking] = useState(false);
  const [id, setId] = useState("");
  const [submitted, setSubmitted] = useState(false);
  const [view, setView] = useState(false);
  const ticket = useRef({ value: 0 });
  const enabled =
    !!context.snapshot?.research?.online && !context.snapshot.research.remote && !!source;
  useEffect(() => {
    const generation = ticket.current;
    ++generation.value;
    setPlan(null);
    setError("");
    setId("");
    setSubmitted(false);
    setWorking(false);
    return () => {
      ++generation.value;
    };
  }, [end, rate, saved?.requests_per_minute]);
  const task = context.snapshot?.research?.tasks.find(task => task.id === id);
  const accepted = submitted || !!task;
  const params = {
    dataset_id: item.id,
    calendar_dataset_id: calendar ?? "",
    mode: calendar ? "repair" : "extend",
    end_day: calendar ? "" : end,
    requests_per_minute: Number(saved?.requests_per_minute ?? rate),
  };
  async function preview() {
    const current = ++ticket.current.value;
    setWorking(true);
    setError("");
    setPlan(null);
    setId("");
    setSubmitted(false);
    try {
      const result = await context.query("research.history.plan", params);
      if (current === ticket.current.value) {
        setPlan(result.history_update_plan ?? null);
        setId(`history-${crypto.randomUUID()}`);
      }
    } catch (e) {
      if (current === ticket.current.value) setError(asDisplayError(e));
    } finally {
      if (current === ticket.current.value) setWorking(false);
    }
  }
  async function submit() {
    if (!plan || !id) return;
    const current = ++ticket.current.value;
    setWorking(true);
    setError("");
    try {
      await context.trade("research.history.submit", {
        id,
        query: plan.query,
        plan_id: plan.id,
        token: saved ? "" : token,
      });
      if (current === ticket.current.value) setSubmitted(true);
    } catch (e) {
      if (current === ticket.current.value) setError(asDisplayError(e));
    } finally {
      setToken("");
      if (current === ticket.current.value) setWorking(false);
    }
  }
  if (view && task?.history_dataset_id)
    return (
      <HistoryDatasetViewer
        {...context}
        archive
        id={task.history_dataset_id}
        source={item.source}
        sourceLabel={item.source}
        timeAxis={item.interval_minutes ? "instant" : "trading-day"}
        onClose={() => setView(false)}
      />
    );
  return (
    <section
      ref={panel}
      className="history-update"
      aria-label={t(calendar ? "补齐缺口" : "下载后续数据")}
    >
      <div className="source-actions">
        <h3>{t(calendar ? "补齐缺口" : "下载后续数据")}</h3>
        <button type="button" disabled={working} onClick={onClose}>
          {t("关闭")}
        </button>
      </div>
      <p>
        {item.contract_id} · {item.interval_minutes ? `${item.interval_minutes}m` : t("日线")} ·{" "}
        {item.source}
      </p>
      <p className="subtle">
        {t("当前版本：{begin} — {end}", { begin: item.begin, end: item.end })}
      </p>
      <p className="subtle">
        {t(
          calendar
            ? "按这两个固定版本核对整日缺口；为保留夜盘边界，将重新下载原始请求时段。数据源不保证补回全部缺失记录。"
            : "从所选版本结束处继续，最多下载到已结束的日期；不会自动合并其他下载或更改已保存数据集。",
        )}
      </p>
      {!source && <p role="status">{t("数据源不可用")}</p>}
      <form
        onSubmit={event => {
          event.preventDefault();
          void preview();
        }}
      >
        <fieldset disabled={!enabled || working || context.busy || accepted}>
          <div className="futures-fields">
            {!calendar && (
              <label>
                {t("下载至日期")}
                <input
                  required
                  type="date"
                  max={yesterday()}
                  value={end}
                  onChange={event => setEnd(event.target.value)}
                />
              </label>
            )}
            {!saved && (
              <label>
                {t("每分钟请求数")}
                <input
                  required
                  type="number"
                  min="1"
                  max={source?.max_requests_per_minute ?? 500}
                  value={rate}
                  onChange={event => setRate(event.target.value)}
                />
              </label>
            )}
          </div>
          <button type="submit" disabled={!!plan}>
            {t("预览下载范围")}
          </button>
        </fieldset>
      </form>
      {plan && (
        <div className="history-update-preview">
          <p>
            <strong>{t("本次下载：{begin} — {end}", { begin: plan.begin, end: plan.end })}</strong>
          </p>
          {calendar && (
            <p>
              {t("待核对 {count} 个交易日：{days}", {
                count: plan.missing_days.length,
                days:
                  plan.missing_days.slice(0, 10).join("、") +
                  (plan.missing_days.length > 10 ? " …" : ""),
              })}
            </p>
          )}
          <p className="subtle">
            {t("生成独立数据版本，原文件保持不变。完成后先检查数据，再用于研究。")}
          </p>
          {!accepted && (
            <div className="source-actions">
              {!saved && source?.credential_required && (
                <label>
                  {source.connection
                    ? getLocale() === "zh-CN"
                      ? source.connection.credential_label_zh
                      : source.connection.credential_label_en
                    : t("数据源凭据")}
                  <input
                    type="password"
                    autoComplete="off"
                    maxLength={source.connection?.credential_max_length ?? 256}
                    value={token}
                    onChange={event => setToken(event.target.value)}
                    disabled={working}
                  />
                </label>
              )}
              <button
                type="button"
                className="primary"
                disabled={
                  !enabled ||
                  working ||
                  context.busy ||
                  (!!source?.credential_required && !saved && !token)
                }
                onClick={() => void submit()}
              >
                {t("开始下载此范围")}
              </button>
            </div>
          )}
          {accepted && <p role="status">{t("下载任务已提交，可在历史数据页查看进度。")}</p>}
          {task?.state === "succeeded" && task.history_dataset_id && (
            <button type="button" onClick={() => setView(true)}>
              {t("查看新版本")}
            </button>
          )}
          {task && ["failed", "interrupted", "cancelled"].includes(task.state) && (
            <p role="alert">{t("本次下载未完成，请在历史数据页查看原因或重试。")}</p>
          )}
        </div>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
