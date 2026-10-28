import { useCallback, useEffect, useMemo, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  lab,
  translate,
  type DisplayError,
  type LabCoverage as CoverageReply,
  type LabSummary as Summary,
  type LabTask as Task,
  type MessageValues,
  type TerminalContext,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const states: Record<string, string> = {
  running: "采集中",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
};

function day(offset: number) {
  const value = new Date(Date.now() + offset * 86400000);
  return value.toISOString().slice(0, 10);
}

// The futures warehouse of the Python lab service: what is stored, by product
// and month, and the acquisitions that fill it.
export function Coverage(context: TerminalContext) {
  const [summary, setSummary] = useState<Summary | null>(null);
  const [coverage, setCoverage] = useState<CoverageReply | null>(null);
  const [tasks, setTasks] = useState<Task[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const [begin, setBegin] = useState(() => day(-30));
  const [end, setEnd] = useState(() => day(-1));
  const [submitting, setSubmitting] = useState(false);

  const refresh = useCallback(async () => {
    try {
      const [nextSummary, nextCoverage, nextTasks] = await Promise.all([
        lab({ case: "summary", value: {} }, "summary"),
        lab({ case: "coverage", value: {} }, "coverage"),
        lab({ case: "tasks", value: {} }, "tasks"),
      ]);
      setSummary(nextSummary);
      setCoverage(nextCoverage);
      setTasks([...nextTasks.tasks].reverse());
      setError("");
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  }, []);
  const running = tasks.some(task => task.state === "running");
  useEffect(() => {
    void refresh();
  }, [refresh]);
  useEffect(() => {
    if (!running) return;
    const timer = setInterval(() => void refresh(), 3000);
    return () => clearInterval(timer);
  }, [running, refresh]);

  const acquire = async () => {
    setSubmitting(true);
    try {
      // The saved credential goes from the keychain to the service natively.
      await context.trade("lab.source.configure", {});
      await lab(
        { case: "acquire", value: { id: `daily-${Date.now()}`, beginDay: begin, endDay: end } },
        "accepted",
      );
      await refresh();
    } catch (failure) {
      setError(asDisplayError(failure));
    } finally {
      setSubmitting(false);
    }
  };
  const cancel = async (id: string) => {
    try {
      await lab({ case: "cancel", value: { id } }, "accepted");
      await refresh();
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  };

  const months = useMemo(
    () =>
      [...new Set(coverage?.products.flatMap(item => item.months.map(m => m.month)) ?? [])].sort(),
    [coverage],
  );
  return (
    <div className="data-workbench data-coverage">
      <ErrorNotice error={error} />
      <section aria-label={t("数据集版本")}>
        <h3>{t("数据集版本")}</h3>
        {summary?.datasetVersion ? (
          <>
            <p>
              <code title={summary.datasetVersion}>{summary.datasetVersion.slice(0, 12)}</code>
            </p>
            <table>
              <thead>
                <tr>
                  <th>{t("表")}</th>
                  <th>{t("行数")}</th>
                  <th>{t("分区")}</th>
                  <th>{t("日期范围")}</th>
                </tr>
              </thead>
              <tbody>
                {summary.tables.map(table => (
                  <tr key={table.table}>
                    <td>
                      {t(
                        { daily: "日线", calendar: "交易日历", contracts: "合约表" }[table.table] ??
                          table.table,
                      )}
                    </td>
                    <td>{table.rows.toString()}</td>
                    <td>{table.partitions}</td>
                    <td>{table.firstDay ? `${table.firstDay} – ${table.lastDay}` : "—"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </>
        ) : (
          <p>{t("仓库里还没有数据。")}</p>
        )}
      </section>
      <section aria-label={t("采集")}>
        <h3>{t("采集")}</h3>
        <p>{t("从 Tushare 采集全部交易所的交易日历、合约表和日线；已入库的日子跳过。")}</p>
        <label>
          {t("开始日期")}
          <input type="date" value={begin} onChange={event => setBegin(event.target.value)} />
        </label>
        <label>
          {t("结束日期")}
          <input type="date" value={end} onChange={event => setEnd(event.target.value)} />
        </label>
        <button type="button" disabled={submitting || running || !begin || !end} onClick={acquire}>
          {t("开始采集")}
        </button>
        {tasks.length > 0 && (
          <table aria-label={t("采集任务")}>
            <thead>
              <tr>
                <th>{t("区间")}</th>
                <th>{t("状态")}</th>
                <th>{t("进度")}</th>
                <th>{t("请求次数")}</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {tasks.map(task => (
                <tr key={task.id}>
                  <td>
                    {task.beginDay} – {task.endDay}
                  </td>
                  <td title={task.error}>
                    {t(states[task.state] ?? task.state)}
                    {task.error ? ` · ${task.error}` : ""}
                  </td>
                  <td>
                    {task.daysDone} / {task.daysTotal}
                    {task.daysEmpty
                      ? ` · ${t("{count} 天来源尚无数据", { count: task.daysEmpty })}`
                      : ""}
                  </td>
                  <td>{task.requests}</td>
                  <td>
                    {task.state === "running" && (
                      <button type="button" onClick={() => void cancel(task.id)}>
                        {t("取消")}
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>
      <section aria-label={t("日线覆盖")}>
        <h3>{t("日线覆盖")}</h3>
        <p>{t("每格为该品种当月有日线的天数 / 交易所开市天数。")}</p>
        {coverage && coverage.products.length > 0 ? (
          <div className="coverage-matrix">
            <table>
              <thead>
                <tr>
                  <th>{t("品种")}</th>
                  {months.map(month => (
                    <th key={month}>{month}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {coverage.products.map(product => {
                  const byMonth = new Map(product.months.map(m => [m.month, m]));
                  return (
                    <tr key={`${product.exchange}/${product.product}`}>
                      <th scope="row">
                        {product.exchange}/{product.product}
                      </th>
                      {months.map(month => {
                        const cell = byMonth.get(month);
                        const share = cell && cell.openDays ? cell.days / cell.openDays : 0;
                        return (
                          <td
                            key={month}
                            data-share={cell ? (share >= 1 ? "full" : "partial") : "none"}
                            title={cell ? `${cell.days} / ${cell.openDays}` : t("无数据")}
                          >
                            {cell ? cell.days : ""}
                          </td>
                        );
                      })}
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        ) : (
          <p>{t("仓库里还没有日线。")}</p>
        )}
      </section>
    </div>
  );
}
