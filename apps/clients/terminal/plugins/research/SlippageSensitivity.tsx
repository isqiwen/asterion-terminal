import { useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceRequestId,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { TaskResult } from "../../src/bridge/client";
import { t, namespace, type Run } from "./shared";
type Evidence = Extract<TaskResult, { kind: "backtest" }>;
export function supportsSlippageSensitivity(value: Evidence) {
  const e = value.experiment;
  return (
    e.paper.contracts.length === 1 &&
    e.paper.contracts[0].dataset.interval_minutes === 0 &&
    !e.series?.length &&
    e.strategies.length === 1 &&
    e.strategies[0].rule.kind === "momentum" &&
    !e.holdout_day &&
    !e.walk_forward
  );
}
export function matchesSlippageBaseline(base: Evidence, value: Evidence, ticks: number) {
  const normalize = (e: Evidence["experiment"]) => ({
    ...e,
    paper: { ...e.paper, contracts: e.paper.contracts.map(c => ({ ...c, slippage_ticks: 0 })) },
  });
  return (
    supportsSlippageSensitivity(base) &&
    value.experiment.paper.contracts[0]?.slippage_ticks === ticks &&
    value.task.metadata?.source_id === base.id &&
    value.task.metadata.source_result_digest === base.task.result_digest &&
    value.task.metadata.data_choice === "fixed" &&
    !value.task.metadata.snapshot &&
    !!base.task.risk_artifact &&
    base.task.risk_artifact === value.task.risk_artifact &&
    base.result.engine_version === value.result.engine_version &&
    base.result.version === value.result.version &&
    JSON.stringify(normalize(base.experiment)) === JSON.stringify(normalize(value.experiment))
  );
}
export function SlippageSensitivity({
  evidence,
  snapshot,
  busy,
  query,
  run,
}: Pick<TerminalContext, "snapshot" | "busy" | "query"> & { evidence: Evidence; run: Run }) {
  const [windows, setWindows] = useState("");
  const [reviewed, setReviewed] = useState(false);
  const [created, setCreated] = useState("");
  const [done, setDone] = useState<number[]>([]);
  const [working, setWorking] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [results, setResults] = useState<Evidence[]>([]);
  const ticks = windows
    .split(",")
    .map(x => x.trim())
    .filter(Boolean)
    .map(Number);
  const baseline = evidence.experiment.paper.contracts[0]?.slippage_ticks;
  const valid =
    ticks.length >= 1 &&
    ticks.length <= 2 &&
    new Set(ticks).size === ticks.length &&
    ticks.every(x => Number.isInteger(x) && x >= 0 && x <= 100 && x !== baseline);
  const [pending, setPending] = useWorkspaceRequestId(
    "slippage-sensitivity",
    JSON.stringify([
      snapshot?.data?.connection_id,
      snapshot?.task_service?.connection_id,
      evidence.id,
      evidence.task.result_digest,
      evidence.input_sha256,
      windows,
    ]),
  );
  const batch = pending ?? created;
  const locked = busy || working || !!batch;
  if (!supportsSlippageSensitivity(evidence)) return null;
  async function create() {
    const batch = pending ?? `slippage-${crypto.randomUUID()}`;
    setPending(batch);
    setWorking(true);
    setError("");
    try {
      for (const value of ticks) {
        if (done.includes(value)) continue;
        if (
          !(await run("backtest.reuse", {
            id: `${batch}-${value}`,
            source_id: evidence.id,
            source_result_digest: evidence.task.result_digest,
            source_input_sha256: evidence.input_sha256,
            mode: "fixed",
            snapshot: "",
            slippage_ticks: value,
          }))
        )
          return;
        setDone(previous => [...previous, value]);
      }
      setCreated(batch);
      setPending(null);
    } finally {
      setWorking(false);
    }
  }
  async function read() {
    setWorking(true);
    setError("");
    setResults([]);
    try {
      const replies = await Promise.all(
        [evidence.id, ...ticks.map(value => `${batch}-${value}`)].map(id =>
          query("task.result.read", { id }),
        ),
      );
      const values = replies.map(reply => {
        if (reply.task_result?.kind !== "backtest")
          throw new Error(t("滑点敏感性证据不匹配，请重新读取原实验。"));
        return reply.task_result;
      });
      const base = values[0];
      if (
        base.input_sha256 !== evidence.input_sha256 ||
        base.task.result_digest !== evidence.task.result_digest ||
        values.slice(1).some((value, i) => !matchesSlippageBaseline(base, value, ticks[i]))
      )
        throw new Error(t("滑点敏感性证据不匹配，请重新读取原实验。"));
      setResults(values);
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setWorking(false);
    }
  }
  return (
    <section aria-label={t("固定数据滑点敏感性")}>
      <h4>{t("固定数据滑点敏感性")}</h4>
      <p>
        {t(
          "仅支持单合约日线、全样本、单个动量策略。固定原数据、策略、风险、资金及费用日程，只改变滑点；不搜索或选择最优策略。",
        )}
      </p>
      <p>
        {t(
          "滑点单位说明：每次成交按当日最小变动价位让出指定跳数；买入更高、卖出更低。金额影响取决于乘数与成交数量，不是手续费或固定金额。成交和累计费用也可能变化。",
        )}
      </p>
      <p>
        {t("原任务")}：<code>{evidence.id}</code> · {t("原滑点（跳）")}：{baseline}
      </p>
      <label>
        {t("滑点变体（1–2个整数，以逗号分隔）")}
        <input
          value={windows}
          placeholder="1,2"
          disabled={locked}
          onChange={event => {
            setWindows(event.target.value);
            setReviewed(false);
          }}
        />
      </label>
      <button disabled={locked || !valid} onClick={() => setReviewed(true)}>
        {t("核对滑点变体")}
      </button>
      {reviewed && (
        <div aria-label={t("滑点变体确认")}>
          <p>
            {t("原值与变体（跳）")}：{baseline} → {ticks.join(", ")}
          </p>
          <p>
            {t("固定输入摘要")}：<code>{evidence.input_sha256}</code>
          </p>
          <p>
            {evidence.experiment.paper.contracts[0].dataset.daily.snapshot} ·{" "}
            {evidence.experiment.paper.contracts[0].dataset.daily.daily_source}
          </p>
          <p>
            {t(
              "只按用户列出的变体分别创建独立任务。部分提交成功后任务保留；继续提交使用原任务ID，不自动补交或回滚。",
            )}
          </p>
          <button
            disabled={
              busy ||
              working ||
              !!created ||
              !snapshot?.task_service?.online ||
              !snapshot?.data?.online
            }
            onClick={() => void create()}
          >
            {pending ? t("继续确认滑点任务") : t("确认创建滑点变体")}
          </button>
          <button
            disabled={locked}
            onClick={() => {
              setReviewed(false);
              setWindows("");
            }}
          >
            {t("取消滑点变体")}
          </button>
        </div>
      )}
      {!!batch && (
        <div>
          {ticks.map(value => {
            const id = `${batch}-${value}`;
            const task = snapshot?.task_service?.tasks.find(x => x.id === id);
            return (
              <p key={id}>
                <code>{id}</code> · {value} · {task?.state ?? t("待确认提交状态")}
              </p>
            );
          })}
          <button disabled={busy || working} onClick={() => void read()}>
            {t("读取滑点敏感性结果")}
          </button>
        </div>
      )}
      {!!results.length && (
        <>
          <p>
            {t(
              "成本假设敏感性：仅滑点变化，其他固定输入和风险实现一致。独立列示原结果，不排序、不宣称最优；通用实验比较的不兼容提示保持不变。",
            )}
          </p>
          <table aria-label={t("滑点敏感性结果")}>
            <thead>
              <tr>
                <th>{t("任务")}</th>
                <th>{t("滑点（跳）")}</th>
                <th>{t("期末权益")}</th>
                <th>{t("累计手续费")}</th>
                <th>{t("总收益率")}</th>
                <th>{t("最大回撤")}</th>
              </tr>
            </thead>
            <tbody>
              {results.map(value => (
                <tr key={value.id}>
                  <td>{value.id}</td>
                  <td>{value.experiment.paper.contracts[0].slippage_ticks}</td>
                  <td>{value.result.account.equity}</td>
                  <td>{value.result.account.fees}</td>
                  <td>{(value.performance.total_return * 100).toFixed(4)}%</td>
                  <td>{(value.performance.max_drawdown * 100).toFixed(4)}%</td>
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}
      {error && <ErrorNotice error={error} namespace={namespace} />}
    </section>
  );
}
