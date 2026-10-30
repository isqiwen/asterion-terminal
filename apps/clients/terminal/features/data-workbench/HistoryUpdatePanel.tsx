import type { CommandParams } from "../../src/bridge/commands";
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
  type DailyTable,
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
  daily,
  onClose,
}: {
  context: TerminalContext;
  item: HistoryDatasetRecord;
  // A repair: the daily table the missing days are found in.
  daily?: DailyTable;
  onClose: () => void;
}) {
  const panel = useRef<HTMLElement>(null);
  useEffect(() => {
    panel.current?.scrollIntoView({ block: "nearest" });
  }, []);
  const [end, setEnd] = useState(yesterday);
  const source = context.snapshot?.data?.sources.find(source => source.id === item.source);
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
  const enabled = !!context.snapshot?.data?.online && !!source;
  const executionReady = enabled;
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
  const task = context.snapshot?.data?.tasks.find(task => task.id === id);
  const accepted = submitted || !!task;
  const params: CommandParams["data.download.update.plan"] = {
    dataset_id: item.id,
    daily: daily ?? null,
    mode: daily ? "repair" : "extend",
    end_day: daily ? "" : end,
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
      const result = await context.query("data.download.update.plan", params);
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
  async function configureBudget() {
    setWorking(true);
    setError("");
    try {
      await context.trade("data.download.budget.configure", {
        source: item.source,
        token: saved ? "" : token,
        requests_per_minute: Number(saved?.requests_per_minute ?? rate),
      });
    } catch (e) {
      setError(asDisplayError(e));
    } finally {
      setWorking(false);
    }
  }
  async function submit() {
    if (!plan || !id) return;
    const current = ++ticket.current.value;
    setWorking(true);
    setError("");
    try {
      await context.trade("data.download.update.submit", {
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
        id={task.history_dataset_id}
        source={item.source}
        sourceLabel={item.source}
        onClose={() => setView(false)}
      />
    );
  return (
    <section
      ref={panel}
      className="history-update"
      aria-label={t(daily ? "补齐缺口" : "下载后续数据")}
    >
      <div className="source-actions">
        <h3>{t(daily ? "补齐缺口" : "下载后续数据")}</h3>
        <button type="button" disabled={working} onClick={onClose}>
          {t("关闭")}
        </button>
      </div>
      <p>
        {item.contract_id} · {`${item.interval_minutes}m`} · {item.source}
      </p>
      <p className="subtle">
        {t("当前版本：{begin} — {end}", { begin: item.begin, end: item.end })}
      </p>
      <p className="subtle">
        {t(
          daily
            ? "按全市场日线表核对整日缺口：表中这个合约有成交、这个版本却没有分钟线的交易日；为保留夜盘边界，将重新下载原始请求时段。数据源不保证补回全部缺失记录。"
            : "从所选版本结束处继续，最多下载到已结束的日期；不会自动合并其他下载或更改已保存数据集。",
        )}
      </p>
      {daily && (
        <p className="subtle">
          {t("公告门禁只覆盖已核实日期；其他日期的历史时段仍未知。通过预览不证明夜盘或日内完整。")}
        </p>
      )}
      {!source && <p role="status">{t("数据源不可用")}</p>}
      <form
        onSubmit={event => {
          event.preventDefault();
          void preview();
        }}
      >
        <fieldset disabled={!enabled || working || context.busy || accepted}>
          <div className="futures-fields">
            {!daily && (
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
      {plan && plan.conflicts.length > 0 && (
        <div role="alert" className="history-update-preview">
          <h4>{t("公告冲突阻止整日补齐预览")}</h4>
          <p>
            {t(
              "所选版本或参考在公告明确休市的日期存在交易记录或开市标记，不能将其作为可补齐缺口。未创建下载任务，也未申请额度。",
            )}
          </p>
          <p>
            {t("固定参考快照")}：<code>{daily?.snapshot}</code>
          </p>
          <table className="data-table">
            <thead>
              <tr>
                {["冲突日期", "冲突依据", "官方休市公告"].map(key => (
                  <th key={key}>{t(key)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {plan.conflicts.map(conflict => (
                <tr key={conflict.day}>
                  <td>{conflict.day}</td>
                  <td>
                    {[
                      conflict.observed_minutes && t("分钟标签包含该交易日"),
                      conflict.calendar_open && t("所选日历标记开市"),
                      conflict.daily_auction && t("所选日线报告竞价成交"),
                    ]
                      .filter(Boolean)
                      .join("；")}
                  </td>
                  <td>
                    <a href={conflict.closure.source_url} target="_blank" rel="noreferrer">
                      {conflict.closure.rule_id} v{conflict.closure.version}
                    </a>{" "}
                    · {t("发布日期")} {conflict.closure.published_day}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <p>
            {t(
              "关闭预览，先到全市场数据核实日历和日线，并检查分钟标签来源。若已有可信更正，重新打开历史数据仓库、覆盖核对并预览新参考快照；没有可信更正时保持阻止，保留旧版本。",
            )}
          </p>
          <button type="button" onClick={onClose}>
            {t("关闭预览并检查参考")}
          </button>
        </div>
      )}
      {plan && plan.conflicts.length === 0 && (
        <div className="history-update-preview">
          <p>
            <strong>{t("本次下载：{begin} — {end}", { begin: plan.begin, end: plan.end })}</strong>
          </p>
          {daily && (
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
            {t("生成独立数据版本，原文件保持不变。完成后先检查数据，再用于回测或因子分析。")}
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
              <p className="subtle">
                {t("同一账号的下载共享当前数据服务的额度；多节点请分别分配份额。")}
              </p>
              <button
                type="button"
                disabled={working || context.busy}
                onClick={() => void configureBudget()}
              >
                {t("应用共享额度")}
              </button>
              <button
                type="button"
                className="primary"
                disabled={
                  !executionReady ||
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
