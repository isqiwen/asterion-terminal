import { HistoryAvailability } from "./HistoryAvailability";
import { useState, useEffect } from "react";
import {
  translate,
  useHistoryDatasets,
  ErrorNotice,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  type TerminalContext,
} from "../contract";
import type { TaskResult, TerminalCommand } from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.research", key);
export function DailyFactorForm({
  snapshot,
  query,
  busy,
  navigate,
  run,
  workspaceParams,
}: Pick<TerminalContext, "query" | "snapshot" | "busy" | "navigate" | "workspaceParams"> & {
  run: (method: TerminalCommand, params: Record<string, unknown>) => Promise<boolean>;
}) {
  const dataConnection = snapshot?.data?.connection_id;
  const destination = JSON.stringify([dataConnection, snapshot?.task_service?.connection_id]);
  const [source, setSource] = useWorkspaceDraft(`daily-source:${destination}`, "");
  const matchesService =
    !workspaceParams?.source_dataset_id || workspaceParams.connection_id === dataConnection;
  useEffect(() => {
    if (workspaceParams?.source_dataset_id)
      setSource(matchesService ? workspaceParams.source_dataset_id : "");
  }, [workspaceParams, matchesService, setSource]);
  const navigationKey = JSON.stringify([destination, workspaceParams]);
  const [acknowledged, setAcknowledged] = useState("");
  const [parameters, setParameters] = useWorkspaceDraft("daily-factor", {
    lookback: "5",
    horizon: "1",
    evaluation: "full_sample",
    split: "",
  });
  const [pending, setPending] = useWorkspaceRequestId(
    "daily-factor-submission",
    JSON.stringify([destination, source, parameters]),
  );
  const [submitted, setSubmitted] = useState("");
  const archive = useHistoryDatasets(snapshot, query);
  const sources = archive.items.filter(item => item.interval_minutes === 0);
  const selected = sources.some(task => task.id === source);
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    const id = pending ?? `daily-factor-${crypto.randomUUID()}`;
    setPending(id);
    if (
      await run("factor.daily.submit", {
        id,
        source_dataset_id: source,
        lookback: Number(parameters.lookback),
        horizon: Number(parameters.horizon),
        evaluation:
          parameters.evaluation === "full_sample"
            ? { mode: "full_sample" }
            : { mode: "holdout", split_index: Number(parameters.split) },
      })
    ) {
      setPending(null);
      setSubmitted(destination);
    }
  }
  return (
    <>
      {!matchesService && acknowledged !== navigationKey && (
        <p role="alert">{t("数据连接已改变，请重新选择当前服务中的日线。")}</p>
      )}
      <h3>{t("日线因子设置")}</h3>
      <p className="subtle">
        {t("按实际日线观测计算收盘价动量，不补齐缺失日期。每个评价分区至少需要 30 个有效样本。")}
      </p>
      {archive.error && (
        <p role="alert">
          <ErrorNotice error={archive.error} />
        </p>
      )}
      {!archive.loading && !sources.length && <p>{t("当前数据服务没有已发布的日线数据。")}</p>}
      <button disabled={busy} onClick={() => navigate("workspace.data", { page: "history" })}>
        {t("下载历史日线")}
      </button>
      <form onSubmit={event => void submit(event)}>
        <fieldset disabled={busy}>
          <label>
            {t("日线来源")}
            <select
              aria-label={t("日线来源")}
              value={selected ? source : ""}
              onChange={event => {
                setSource(event.target.value);
                setAcknowledged(navigationKey);
                setSubmitted("");
              }}
            >
              <option value="">{t("选择日线数据版本")}</option>
              {sources.map(task => (
                <option key={task.id} value={task.id}>
                  {task.contract_id} · {task.begin} — {task.end} · {task.source} ·{" "}
                  {task.id.slice(0, 8)}
                </option>
              ))}
            </select>
          </label>
          <div className="research-fields">
            {(["lookback", "horizon"] as const).map(key => (
              <label key={key}>
                {t(key === "lookback" ? "回看日线数" : "未来日线数")}
                <input
                  type="number"
                  min="1"
                  max="10000"
                  step="1"
                  required
                  value={parameters[key]}
                  onChange={event => setParameters({ ...parameters, [key]: event.target.value })}
                />
              </label>
            ))}
          </div>
          <label>
            {t("评价方式")}
            <select
              aria-label={t("评价方式")}
              value={parameters.evaluation}
              onChange={event => setParameters({ ...parameters, evaluation: event.target.value })}
            >
              <option value="full_sample">{t("样本内评价")}</option>
              <option value="holdout">{t("时间留出评价")}</option>
            </select>
          </label>
          {parameters.evaluation === "holdout" && (
            <label>
              {t("前段日线数")}
              <input
                type="number"
                min="1"
                max="7321"
                step="1"
                required
                value={parameters.split}
                onChange={event => setParameters({ ...parameters, split: event.target.value })}
              />
            </label>
          )}
          <p className="subtle">{t("未来收益只用于评价；跨分界标签剔除。留出参数应事先固定。")}</p>
          <button
            className="primary"
            type="submit"
            disabled={!snapshot?.task_service?.online || !snapshot?.data?.online || !selected}
          >
            {pending ? t("确认提交状态") : t("开始日线分析")}
          </button>
        </fieldset>
      </form>
      {submitted === destination && (
        <p role="status" className="subtle">
          {t("任务已提交，可在任务中心查看进度。")}
        </p>
      )}
    </>
  );
}
export function DailyFactorResults({
  evidence,
}: {
  evidence: Extract<TaskResult, { kind: "daily_factor" }>;
}) {
  const { experiment, result } = evidence;
  const [page, setPage] = useState(0);
  const last = Math.max(0, Math.ceil(result.samples.length / 50) - 1);
  const current = Math.min(page, last);
  const correlation = (value: number | null) => (value === null ? t("未定义") : value.toFixed(4));
  return (
    <section className="research-result" aria-label={t("日线因子结果")}>
      <h3>{t("日线因子结果")}</h3>
      <HistoryAvailability versions={[experiment.data.history_evidence]} />
      <p>
        {experiment.data.contract_id} · {experiment.data.first_day} — {experiment.data.last_day}
      </p>
      <p className="subtle">
        {t(experiment.evaluation.mode === "holdout" ? "时间留出评价" : "样本内评价")} ·{" "}
        {t("回看日线数")} {experiment.lookback} · {t("未来日线数")} {experiment.horizon}
      </p>
      <p>
        {t("输入日线")}: {result.input_count} · {t("有效样本")}: {result.samples.length} ·{" "}
        {t("跨界剔除")}: {result.purged_count}
      </p>
      <div className="research-table">
        <table aria-label={t("日线分区评价")}>
          <thead>
            <tr>
              <th>{t("分区")}</th>
              <th>{t("有效样本")}</th>
              <th>Pearson</th>
              <th>Spearman</th>
            </tr>
          </thead>
          <tbody>
            {result.partitions.map(part => (
              <tr key={part.name}>
                <td>
                  {t(
                    part.name === "full_sample"
                      ? "全样本"
                      : part.name === "development"
                        ? "前段"
                        : "留出段",
                  )}
                </td>
                <td>{part.sample_count}</td>
                <td>{correlation(part.pearson)}</td>
                <td>{correlation(part.spearman)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <details>
        <summary>{t("实验详情")}</summary>
        <p>
          {t("来源数据版本")}: {experiment.data.source_dataset_id}
        </p>
        <p>{experiment.data.source}</p>
        <p>
          {t("数据版本")}: {experiment.dataset_revision}
        </p>
        <p>
          {t("来源摘要")}: {experiment.data.manifest_sha256}
        </p>
        <p>{result.engine_version}</p>
      </details>
      <details>
        <summary>{t("逐日样本")}</summary>
        <div className="research-table">
          <table>
            <thead>
              <tr>
                <th>{t("交易日")}</th>
                <th>{t("标签日期")}</th>
                <th>{t("动量")}</th>
                <th>{t("未来收益")}</th>
              </tr>
            </thead>
            <tbody>
              {result.samples.slice(current * 50, current * 50 + 50).map(row => (
                <tr key={row.observation_index}>
                  <td>{row.trading_day}</td>
                  <td>{row.label_day}</td>
                  <td>{(row.value * 100).toFixed(4)}%</td>
                  <td>{(row.forward_return * 100).toFixed(4)}%</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <button disabled={current === 0} onClick={() => setPage(current - 1)}>
          {t("上一页")}
        </button>
        <span>
          {" "}
          {current + 1} / {last + 1}{" "}
        </span>
        <button disabled={current === last} onClick={() => setPage(current + 1)}>
          {t("下一页")}
        </button>
      </details>
    </section>
  );
}
