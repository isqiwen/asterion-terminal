import { useState } from "react";
import {
  taskOwner,
  type ResourceObject,
  type StatisticsRequest,
  type StatisticValue,
} from "../../src/bridge/client";
import {
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type TerminalContext,
} from "../contract";
import { ResearchPage, TaskRecords, t, useRun, variableUnit } from "./shared";

type Source = {
  object: ResourceObject;
  count: number;
  role: StatisticsRequest["role"];
  paired: number | null;
  program: string;
};
type Draft = {
  source: Source | null;
  purpose: string;
  column: StatisticsRequest["column"];
  begin: string;
  end: string;
  bins: string;
  grouping: NonNullable<StatisticsRequest["grouping"]>;
};
const scalar = (value: StatisticValue) =>
  value.state === "DEFINED" && value.value !== null ? String(value.value) : t(value.state);
export function ResearchStatistics({ snapshot, query, trade, busy }: TerminalContext) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([service?.node_id, service?.service]);
  const [draft, setDraft] = useWorkspaceDraft<Draft>(`statistics-draft:${destination}`, {
    source: null,
    purpose: "",
    column: "feature",
    begin: "0",
    end: "",
    bins: "8",
    grouping: "none",
  });
  const [sourceId, setSourceId] = useWorkspaceDraft(`statistics-source:${destination}`, "");
  const [operation, setOperation] = useWorkspaceRequestId(
    `statistics-submit:${destination}`,
    JSON.stringify(draft),
  );
  const [loading, setLoading] = useState(false);
  const [bin, setBin] = useState<number | null>(null);
  const [day, setDay] = useState("");
  const [page, setPage] = useState(0);
  const { error, setError, run } = useRun(trade);
  useWorkspaceUnsaved(
    `statistics-draft:${destination}`,
    Boolean(draft.source && draft.purpose && !operation),
  );
  const blocked = busy || loading || !service?.online;
  const locked = blocked || operation !== null;
  const integer = (value: string) => /^\d+$/.test(value) && Number.isSafeInteger(Number(value));
  const valid =
    draft.source &&
    draft.purpose.trim() &&
    integer(draft.begin) &&
    integer(draft.end) &&
    integer(draft.bins) &&
    Number(draft.begin) < Number(draft.end) &&
    Number(draft.end) <= draft.source.count &&
    Number(draft.bins) >= 1 &&
    Number(draft.bins) <= 64;
  const report =
    snapshot?.task_result?.kind === "statistics" &&
    snapshot.task_result.result.owner.node_id === service?.owner_node_id &&
    snapshot.task_result.result.owner.service_id === service?.service
      ? snapshot.task_result
      : null;
  async function selectSource() {
    if (!service) return;
    setLoading(true);
    setError("");
    try {
      const value = (
        await query("task.result.read", { id: sourceId, owner: taskOwner(service, "research") })
      ).task_result;
      if (
        !value ||
        value.task.state !== "succeeded" ||
        value.task.retired ||
        !value.task.program_artifact
      )
        throw new Error(t("请选择保留逐行证据与原程序的正式研究结果。"));
      let source: Source;
      if (value.kind === "sample") {
        source = {
          object: {
            node: value.result.owner.node_id,
            service: value.result.owner.service_id,
            domain: "research",
            kind: "sample_result",
            id: value.id,
            version: value.task.result_digest,
          },
          count: value.result.rows.length,
          role:
            value.experiment.request.evaluation.mode === "holdout" ? "development" : "full_sample",
          paired: null,
          program: value.task.program_artifact,
        };
      } else if (
        value.kind === "factor" &&
        value.result.observations?.length &&
        value.experiment.series.length === 1 &&
        value.experiment.lookbacks.length === 1 &&
        value.result.selection_rule === "fixed" &&
        value.experiment.evaluation.mode !== "walk_forward"
      ) {
        source = {
          object: {
            node: service.owner_node_id ?? "",
            service: service.service,
            domain: "research",
            kind: "factor_result",
            id: value.id,
            version: value.task.result_digest,
          },
          count: value.result.observations.length,
          role: value.experiment.evaluation.mode === "holdout" ? "development" : "full_sample",
          paired: value.result.partitions[0].sample_count,
          program: value.task.program_artifact,
        };
      } else throw new Error(t("请选择保留逐行证据与原程序的正式研究结果。"));
      setDraft({
        ...draft,
        source,
        column: source.object.kind === "sample_result" ? "raw_close" : "feature",
        grouping: "none",
        begin: "0",
        end: String(source.count),
      });
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setLoading(false);
    }
  }
  async function submit() {
    if (!valid || !service || !draft.source) return;
    const id = operation ?? `statistics-${crypto.randomUUID()}`;
    setOperation(id);
    setLoading(true);
    try {
      await run("statistics.submit", {
        id,
        owner: taskOwner(service, "research"),
        source: draft.source.object,
        use_scope: draft.purpose,
        column: draft.column,
        role: draft.source.role,
        begin_index: Number(draft.begin),
        end_index: Number(draft.end),
        bins: Number(draft.bins),
        grouping: draft.grouping,
      });
    } finally {
      setLoading(false);
    }
  }
  const selectedGroup = report?.result.groups.find(group => group.trading_day === day);
  const summary = selectedGroup?.summary ?? report?.result.summary;
  const reasons = selectedGroup?.reasons ?? report?.result.reasons ?? [];
  const bins =
    report?.result.bins.map((bucket, index) => ({
      ...bucket,
      count: selectedGroup ? selectedGroup.bin_counts[index] : bucket.count,
    })) ?? [];
  const selectedRows =
    report?.result.rows.filter(
      row =>
        (!selectedGroup || row.trading_day === selectedGroup.trading_day) &&
        (bin === null || row.bin === bin),
    ) ?? [];
  const last = Math.max(0, Math.ceil(selectedRows.length / 50) - 1);
  const current = Math.min(page, last);
  return (
    <ResearchPage taskService={service} error={error}>
      <p>
        {t("读取固定研究样本或原因子证据；不重算因子，不用未来标签筛选同期特征，不探索留出区间。")}
      </p>
      <fieldset disabled={locked}>
        <label>
          {t("来源研究结果")}
          <select
            aria-label={t("来源研究结果")}
            value={sourceId}
            onChange={event => setSourceId(event.target.value)}
          >
            <option value="">{t("选择正式结果")}</option>
            {service?.tasks
              .filter(
                task =>
                  (task.kind === "factor" || task.kind === "sample") &&
                  task.state === "succeeded" &&
                  !task.retired,
              )
              .map(task => (
                <option key={task.id} value={task.id}>
                  {task.id}
                </option>
              ))}
          </select>
        </label>
        <button disabled={!sourceId} onClick={() => void selectSource()}>
          {t("固定原结果")}
        </button>
        {draft.source && (
          <>
            <code>
              {draft.source.object.id}/{draft.source.object.version}
            </code>
            {draft.source.paired !== null && (
              <p>
                {t("来源评价配对数：{count}；统计列独立确定资格，不沿用配对筛选。", {
                  count: draft.source.paired,
                })}
              </p>
            )}
            <p>
              {t("固定执行程序")}：<code>{draft.source.program}</code>
            </p>
          </>
        )}
        <label>
          {t("统计用途")}
          <input
            value={draft.purpose}
            onChange={event => setDraft({ ...draft, purpose: event.target.value })}
          />
        </label>
        <label>
          {t("统计列")}
          <select
            value={draft.column}
            onChange={event =>
              setDraft({ ...draft, column: event.target.value as StatisticsRequest["column"] })
            }
          >
            {draft.source?.object.kind === "sample_result" ? (
              <option value="raw_close">{t("原始收盘价")}</option>
            ) : (
              <>
                <option value="feature">{t("同期特征")}</option>
                <option value="forward_return">{t("未来收益（比例）")}</option>
              </>
            )}
          </select>
        </label>
        <p>
          {t("统计角色")}：
          {draft.source ? t(draft.source.role === "development" ? "开发区间" : "全样本") : "—"}
        </p>
        <label>
          {t("统计分组")}
          <select
            value={draft.grouping}
            onChange={event =>
              setDraft({
                ...draft,
                grouping: event.target.value as NonNullable<StatisticsRequest["grouping"]>,
              })
            }
          >
            <option value="none">{t("不分组")}</option>
            <option value="trading_day" disabled={draft.source?.object.kind !== "sample_result"}>
              {t("原交易日")}
            </option>
          </select>
        </label>
        <label>
          {t("起始观察索引（含）")}
          <input
            type="number"
            min="0"
            value={draft.begin}
            onChange={event => setDraft({ ...draft, begin: event.target.value })}
          />
        </label>
        <label>
          {t("结束观察索引（不含）")}
          <input
            type="number"
            min="1"
            value={draft.end}
            onChange={event => setDraft({ ...draft, end: event.target.value })}
          />
        </label>
        <label>
          {t("直方图区间数")}
          <input
            type="number"
            min="1"
            max="64"
            value={draft.bins}
            onChange={event => setDraft({ ...draft, bins: event.target.value })}
          />
        </label>
      </fieldset>
      <button disabled={blocked || !valid} onClick={() => void submit()}>
        {t("提交或确认原统计任务")}
      </button>
      {operation && (
        <p>
          {t("统计任务身份")}：<code>{operation}</code>
          <button disabled={blocked} onClick={() => setOperation(null)}>
            {t("新建统计请求")}
          </button>
        </p>
      )}
      <TaskRecords
        taskService={service}
        busy={busy}
        query={query}
        trade={trade}
        run={run}
        listed={task => task.kind === "statistics"}
        onResult={task => {
          setBin(null);
          setDay("");
          setPage(0);
          void run("task.result", { id: task.id, owner: taskOwner(service, "research") });
        }}
      />
      {report && summary && (
        <section aria-label={t("正式统计报告")}>
          <h3>{t("正式统计报告")}</h3>
          {report.result.source.kind === "sample_result" && (
            <p>{t("事后探索；历史可得时间未知，完整历史覆盖未获证明。")}</p>
          )}
          {report.result.groups.length > 0 && (
            <section aria-label={t("原交易日分组")}>
              <label>
                {t("查看已保存交易日分组")}
                <select
                  value={selectedGroup?.trading_day ?? ""}
                  onChange={event => {
                    setDay(event.target.value);
                    setBin(null);
                    setPage(0);
                  }}
                >
                  <option value="">{t("全部交易日（原总体）")}</option>
                  {report.result.groups.map(group => (
                    <option key={group.trading_day} value={group.trading_day}>
                      {group.trading_day} · {group.summary.valid}
                    </option>
                  ))}
                </select>
              </label>
              <p>{t("分组与总体共用原分箱边界；选择分组只读取保存结果，不按UTC自然日重分组。")}</p>
            </section>
          )}
          <code>{report.id}</code>
          <p>
            {t("固定来源")}：
            <code>
              {report.result.source.id}/{report.result.source.version}
            </code>
          </p>
          <p>
            {t("统计用途")}：{report.experiment.request.use_scope}
          </p>
          <p>
            {t("统计列")}：
            {t(
              report.experiment.request.column === "raw_close"
                ? "原始收盘价"
                : report.experiment.request.column === "feature"
                  ? "同期特征"
                  : "未来收益（比例）",
            )}{" "}
            · {variableUnit(report.result.price_power)}
          </p>
          <p>
            {t("原计算程序")}：<code>{report.experiment.source.program_artifact}</code>
          </p>
          <p>
            {t("统计执行程序")}：<code>{report.task.program_artifact}</code>
          </p>
          <dl>
            {(
              [
                ["所选观察数", summary.total],
                ["角色内观察数", summary.allowed],
                ["实际有效数", summary.valid],
                ["角色内缺失数", summary.missing],
              ] as const
            ).map(([label, value]) => (
              <div key={label}>
                <dt>{t(label)}</dt>
                <dd>{value}</dd>
              </div>
            ))}
          </dl>
          <dl>
            {(
              [
                ["均值", summary.mean],
                ["样本标准差", summary.standard_deviation],
                ["最小值", summary.minimum],
                ["第一四分位数", summary.q25],
                ["中位数", summary.median],
                ["第三四分位数", summary.q75],
                ["最大值", summary.maximum],
              ] as const
            ).map(([label, value]) => (
              <div key={label}>
                <dt>{t(label)}</dt>
                <dd>{scalar(value)}</dd>
              </div>
            ))}
          </dl>
          <p>
            {t(
              "缺失不补零；分位数使用 Type 7，标准差使用 n−1。数值按原单位显示，无量纲不自动转百分比。",
            )}
          </p>
          <ul>
            {reasons.map(item => (
              <li key={item.reason}>
                {t(item.reason)}：{item.count}
              </li>
            ))}
          </ul>
          <section aria-label={t("原报告直方图")}>
            <h4>{t("原报告直方图")}</h4>
            <button
              onClick={() => {
                setBin(null);
                setPage(0);
              }}
            >
              {t("显示全部观察")}
            </button>
            {bins.map((item, index) => (
              <button
                key={index}
                aria-pressed={bin === index}
                onClick={() => {
                  setBin(index);
                  setPage(0);
                }}
              >
                <meter min="0" max={Math.max(1, summary.valid)} value={item.count} />
                {index}: [{item.lower}, {item.upper}
                {index === bins.length - 1 ? "]" : ")"} · {item.count}
              </button>
            ))}
          </section>
          <p>{t("逐行值、原因与分箱均读取原报告；时间保留原精度，不推断历史可得时点。")}</p>
          <table aria-label={t("统计逐行证据")}>
            <thead>
              <tr>
                {[
                  "观察序号",
                  "原观察时间/交易日",
                  "原统计值",
                  "资格原因",
                  "原分箱",
                  "原交易日",
                ].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {selectedRows.slice(current * 50, current * 50 + 50).map(row => (
                <tr key={row.event_index}>
                  <td>{row.event_index}</td>
                  <td>{row.observed}</td>
                  <td>{row.value === null ? "—" : String(row.value)}</td>
                  <td>{t(row.reason)}</td>
                  <td>{row.bin ?? "—"}</td>
                  <td>{row.trading_day || "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <button disabled={current === 0} onClick={() => setPage(current - 1)}>
            {t("上一页")}
          </button>
          <span>
            {current + 1}/{last + 1}
          </span>
          <button disabled={current === last} onClick={() => setPage(current + 1)}>
            {t("下一页")}
          </button>
        </section>
      )}
    </ResearchPage>
  );
}
