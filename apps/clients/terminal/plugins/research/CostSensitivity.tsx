import { useRef, useState } from "react";
import {
  taskOwner,
  type ResearchBatchDraft,
  type ResearchBatchReply,
  type ResearchBatchState,
  type TaskResult,
} from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { t, namespace, retiredResearch, states } from "./shared";
type Evidence = Extract<TaskResult, { kind: "backtest" }>;
export function supportsCostSensitivity(value: Evidence) {
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
export function matchesCostBaseline(
  base: Evidence,
  value: Evidence,
  ticks: number,
  feeMultiplier?: string,
) {
  const normalize = (e: Evidence["experiment"]) => ({
    ...e,
    paper: {
      ...e.paper,
      contracts: e.paper.contracts.map(c => ({
        ...c,
        slippage_ticks: feeMultiplier === undefined ? 0 : c.slippage_ticks,
        cost_schedule:
          feeMultiplier === undefined
            ? c.cost_schedule
            : c.cost_schedule.map(v => ({
                ...v,
                values: {
                  ...v.values,
                  open_fee: "0",
                  close_today_fee: "0",
                  close_yesterday_fee: "0",
                  open_fee_rate: "0",
                  close_today_fee_rate: "0",
                  close_yesterday_fee_rate: "0",
                },
              })),
      })),
    },
  });
  return (
    supportsCostSensitivity(base) &&
    value.experiment.paper.contracts[0]?.slippage_ticks === ticks &&
    value.task.metadata?.source_id === base.id &&
    value.task.metadata.source_result_digest === base.task.result_digest &&
    value.task.metadata.data_choice === "fixed" &&
    !value.task.metadata.snapshot &&
    !!base.task.risk_artifact &&
    base.task.risk_artifact === value.task.risk_artifact &&
    JSON.stringify(normalize(base.experiment)) === JSON.stringify(normalize(value.experiment))
  );
}

export function CostSensitivity({
  evidence,
  snapshot,
  busy,
  query,
  trade,
}: Pick<TerminalContext, "snapshot" | "busy" | "query" | "trade"> & { evidence: Evidence }) {
  const service = snapshot?.research_service;
  const scope = JSON.stringify([
    service?.owner_node_id,
    service?.service,
    service?.connection_id,
    evidence.id,
    evidence.task.result_digest,
    evidence.input_sha256,
  ]);
  const current = useRef(scope);
  current.current = scope;
  const [editor, setEditor] = useWorkspaceDraft(`batch-editor:${scope}`, {
    ticks: "",
    fees: "",
    kind: "slippage" as "slippage" | "fees",
    name: "",
    purpose: "",
    limitations: "",
    seconds: "",
  });
  const [draft, setDraft] = useWorkspaceDraft<ResearchBatchDraft | null>(
    `batch-preview:${scope}`,
    null,
  );
  const [reply, setReply] = useWorkspaceDraft<ResearchBatchReply | null>(
    `batch-selected:${scope}`,
    null,
  );
  const [items, setItems] = useWorkspaceDraft<ResearchBatchState[]>(`batch-list:${scope}`, []);
  const [after, setAfter] = useWorkspaceDraft(`batch-cursor:${scope}`, "");
  const [reason, setReason] = useWorkspaceDraft(`batch-stop-reason:${scope}`, "");
  const [working, setWorking] = useState<string | null>(null);
  const [error, setError] = useWorkspaceDraft<DisplayError>(`batch-error:${scope}`, "");
  const [results, setResults] = useWorkspaceDraft<Evidence[]>(`batch-results:${scope}`, []);
  const [saveId, setSaveId] = useWorkspaceRequestId(`batch-save:${scope}`, JSON.stringify(draft));
  const selected = reply?.state;
  const [startId, setStartId] = useWorkspaceRequestId(
    `batch-start:${scope}`,
    JSON.stringify(selected?.definition.object ?? null),
  );
  const [stopId, setStopId] = useWorkspaceRequestId(
    `batch-stop:${scope}`,
    JSON.stringify([selected?.definition.object, reason]),
  );
  const baseline = evidence.experiment.paper.contracts[0]?.slippage_ticks;
  const ticks = editor.ticks
      .split(",")
      .map(x => x.trim())
      .filter(Boolean)
      .map(Number),
    seconds = Number(editor.seconds);
  const feeMode = editor.kind === "fees";
  const feeValues = (editor.fees ?? "")
    .split(",")
    .map(x => x.trim())
    .filter(Boolean);
  const feeUnits = feeValues.map(value => {
    if (!/^(0|[1-9]\d{0,2})(\.\d{1,8})?$/.test(value)) return null;
    const [whole, fraction = ""] = value.split(".");
    return BigInt(whole) * 100000000n + BigInt(fraction.padEnd(8, "0"));
  });
  const validFees =
    feeValues.length > 0 &&
    feeValues.length <= 100 &&
    feeUnits.every(value => value !== null && value <= 10000000000n && value !== 100000000n) &&
    new Set(feeUnits.map(value => value?.toString())).size === feeValues.length;
  const validSlippage =
    ticks.length >= 1 &&
    ticks.length <= 100 &&
    new Set(ticks).size === ticks.length &&
    ticks.every(x => Number.isInteger(x) && x >= 0 && x <= 100 && x !== baseline);
  const valid =
    (feeMode ? validFees : validSlippage) &&
    editor.name.trim() &&
    editor.purpose.trim() &&
    editor.limitations.trim() &&
    Number.isInteger(seconds) &&
    seconds > 0 &&
    seconds <= 4294967295;
  const blocked = busy || working === scope || !service?.online;
  const retired = retiredResearch(snapshot, evidence.task);
  function check(owner: string) {
    if (current.current !== owner)
      throw new Error(t("研究服务或原实验已切换，请在原服务核对批次状态。"));
  }
  function accept(value: ResearchBatchReply | undefined) {
    const state = value?.state,
      source = state?.definition.source;
    if (
      !value ||
      !state ||
      source?.id !== evidence.id ||
      source.version !== evidence.task.result_digest ||
      source.node !== service?.owner_node_id ||
      source.service !== service?.service ||
      state.definition.source_input_sha256 !== evidence.input_sha256
    )
      throw new Error(t("滑点敏感性证据不匹配，请重新读取原实验。"));
    setReply(value);
    setResults([]);
  }
  async function perform(action: (owner: string) => Promise<void>) {
    const owner = scope;
    setWorking(owner);
    setError("");
    try {
      await action(owner);
    } catch (value) {
      if (current.current === owner) setError(asDisplayError(value));
    } finally {
      setWorking(value => (value === owner ? null : value));
    }
  }
  async function save() {
    if (!draft) return;
    const id = saveId ?? crypto.randomUUID();
    setSaveId(id);
    await perform(async owner => {
      await trade("research.batch.save", { id, definition: draft });
      check(owner);
      const value = await query("research.batch.read", { id: draft.id });
      check(owner);
      accept(value.research_batch);
      setSaveId(null);
      setDraft(null);
    });
  }
  async function start() {
    if (!selected) return;
    const id = startId ?? crypto.randomUUID();
    setStartId(id);
    await perform(async owner => {
      await trade("research.batch.start", { id, batch: selected.definition.object });
      check(owner);
      const value = await query("research.batch.read", { id: selected.definition.object.id });
      check(owner);
      accept(value.research_batch);
      setStartId(null);
    });
  }
  async function stop() {
    if (!selected) return;
    const id = stopId ?? crypto.randomUUID();
    setStopId(id);
    await perform(async owner => {
      await trade("research.batch.stop", {
        id,
        batch: selected.definition.object,
        reason,
      });
      check(owner);
      const value = await query("research.batch.read", { id: selected.definition.object.id });
      check(owner);
      accept(value.research_batch);
      setStopId(null);
    });
  }
  async function list(cursor = "") {
    await perform(async owner => {
      const value = (await query("research.batch.list", { after: cursor, limit: 20 }))
        .research_batch;
      check(owner);
      if (!value) throw new Error(t("批次响应缺失"));
      setItems(previous => (cursor ? [...previous, ...value.states] : value.states));
      setAfter(value.next_after);
    });
  }
  async function read(id: string) {
    await perform(async owner => {
      const value = (await query("research.batch.read", { id })).research_batch;
      check(owner);
      accept(value);
    });
  }
  async function readResults() {
    if (!selected || !reply) return;
    await perform(async owner => {
      const values: Evidence[] = [evidence];
      for (const task of reply.members.filter(task => task.state === "succeeded")) {
        const value = (
          await query("task.result.read", { owner: taskOwner(service, "research"), id: task.id })
        ).task_result;
        check(owner);
        const member = selected.definition.members.find(member => member.task_id === task.id),
          batch = value?.task.metadata?.batch;
        const object = selected.definition.object;
        if (
          !value ||
          value.kind !== "backtest" ||
          !member ||
          !matchesCostBaseline(evidence, value, member.slippage_ticks, member.fee_multiplier) ||
          selected.members.find(row => row.task_id === task.id)?.input_sha256 !==
            value.input_sha256 ||
          value.task.program_artifact !== selected.definition.program_artifact ||
          !batch ||
          batch.id !== object.id ||
          batch.version !== object.version ||
          batch.node !== object.node ||
          batch.service !== object.service ||
          batch.kind !== object.kind ||
          batch.domain !== object.domain
        )
          throw new Error(t("滑点敏感性证据不匹配，请重新读取原实验。"));
        values.push(value);
      }
      setResults(values);
    });
  }
  if (!supportsCostSensitivity(evidence)) return null;
  return (
    <section aria-label={t("固定数据成本敏感性")}>
      <h4>{t("固定数据成本敏感性")}</h4>
      <p>
        {t(
          "仅支持单合约日线、全样本、单个动量策略。明确选择只改变滑点或六项手续费倍率，固定原数据、策略、风险、资金及保证金；不搜索或选择最优策略。",
        )}
      </p>
      <p>
        {t(
          "原基准已经观察；本批次是事后成本敏感性，不是独立验证、选优或实盘许可。保存不执行，确认启动后由 Research 顺序接纳，关闭页面不会停止任务。",
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
      {retired && <p role="status">{t("原记录已退役，不能新增派生实验")}</p>}
      <label>
        {t("成本情景类型")}
        <select
          value={editor.kind ?? "slippage"}
          disabled={blocked || !!draft || !!saveId}
          onChange={event =>
            setEditor(previous => ({
              ...previous,
              kind: event.target.value as "slippage" | "fees",
            }))
          }
        >
          <option value="slippage">{t("仅滑点变化")}</option>
          <option value="fees">{t("仅手续费变化")}</option>
        </select>
      </label>
      <p>
        {t(
          "手续费情景精确缩放开仓、平今和平昨的每手金额与名义金额比例，保证金和滑点不变；不是供应商真实费率，账户将实际重跑。",
        )}
      </p>
      {(
        [
          ["name", "批次名称"],
          ["purpose", "批次用途"],
          ["limitations", "批次局限"],
          ["ticks", "滑点变体（1–100个整数，以逗号分隔）"],
          ["fees", "手续费倍率（1–100个精确小数，以逗号分隔）"],
          ["seconds", "时间预算（秒，必填）"],
        ] as const
      )
        .filter(([key]) => key !== (feeMode ? "ticks" : "fees"))
        .map(([key, label]) => (
          <label key={key}>
            {t(label)}
            <input
              value={editor[key]}
              disabled={blocked || !!draft || !!saveId || retired}
              onChange={event =>
                setEditor(previous => ({ ...previous, [key]: event.target.value }))
              }
            />
          </label>
        ))}
      <button
        disabled={blocked || retired || !valid || !!draft || !!saveId}
        onClick={() =>
          setDraft({
            id: `costs-${crypto.randomUUID()}`,
            name: editor.name,
            purpose: editor.purpose,
            limitations: editor.limitations,
            source: {
              domain: "research",
              node: service!.owner_node_id!,
              service: service!.service,
              kind: "backtest_result",
              id: evidence.id,
              version: evidence.task.result_digest,
            },
            members: feeMode
              ? feeValues.map(value => ({
                  task_id: `cost-${crypto.randomUUID()}`,
                  slippage_ticks: baseline,
                  fee_multiplier: value,
                }))
              : ticks.map(value => ({
                  task_id: `cost-${crypto.randomUUID()}`,
                  slippage_ticks: value,
                })),
            run_budget: feeMode ? feeValues.length : ticks.length,
            time_budget_seconds: seconds,
          })
        }
      >
        {t(feeMode ? "核对手续费变体" : "核对滑点变体")}
      </button>
      {draft && (
        <div
          aria-label={t(
            draft.members.some(x => x.fee_multiplier !== undefined)
              ? "手续费变体确认"
              : "滑点变体确认",
          )}
        >
          <p>
            {t("原值与变体（跳）")}：{baseline} →{" "}
            {draft.members
              .map(x =>
                x.fee_multiplier === undefined
                  ? `${x.slippage_ticks}`
                  : `${x.slippage_ticks} · ${t("手续费倍率（合成情景）")}: ${x.fee_multiplier}`,
              )
              .join(", ")}
          </p>
          <p>
            {t("固定输入摘要")}：<code>{evidence.input_sha256}</code>
          </p>
          <p>
            {t("次数预算")}：{draft.run_budget} · {t("时间预算（秒，必填）")}：
            {draft.time_budget_seconds}
          </p>
          <ul aria-label={t("预览全部成员身份")}>
            {draft.members.map(member => (
              <li key={member.task_id}>
                <code>{member.task_id}</code> · {member.slippage_ticks} ·{" "}
                {t("手续费倍率（合成情景）")}：{member.fee_multiplier ?? "1"}
              </li>
            ))}
          </ul>
          <p>
            {t("原结果版本")}：<code>{draft.source.version}</code>
          </p>
          <p>
            {t("原执行程序")}：<code>{evidence.task.program_artifact}</code> · {t("原风险实现")}：
            <code>{evidence.task.risk_artifact}</code>
          </p>
          <p>
            {t("每次原任务事件数")}：{evidence.task.total} · {t("计划事件工作量")}：
            {Number.isSafeInteger(evidence.task.total) && evidence.task.total > 0
              ? (BigInt(evidence.task.total) * BigInt(draft.members.length)).toString()
              : t("未知")}
          </p>
          <p>{t("默认顺序执行一个成员；未估算运行时长。原基准不计入新运行数。")}</p>
          <button disabled={blocked || retired} onClick={() => void save()}>
            {saveId ? t("确认批次保存状态") : t("保存批次（不执行）")}
          </button>
          <button disabled={blocked || !!saveId} onClick={() => setDraft(null)}>
            {t(feeMode ? "取消手续费变体" : "取消滑点变体")}
          </button>
        </div>
      )}
      <button disabled={blocked} onClick={() => void list()}>
        {t("读取已有批次")}
      </button>
      {!!after && (
        <button disabled={blocked} onClick={() => void list(after)}>
          {t("更多批次")}
        </button>
      )}
      {items
        .filter(
          item =>
            item.definition.source.id === evidence.id &&
            item.definition.source.version === evidence.task.result_digest,
        )
        .map(item => (
          <button
            key={item.definition.object.id}
            disabled={blocked}
            onClick={() => void read(item.definition.object.id)}
          >
            {item.definition.name} · {item.definition.object.id}
          </button>
        ))}
      {selected && (
        <div aria-label={t("持久批次状态")}>
          <p>
            <code>{selected.definition.object.id}</code> · {t("批次状态")}：
            {t(`batch.phase.${selected.phase}`)}
          </p>
          <p>
            {selected.definition.purpose} · {selected.definition.limitations}
          </p>
          <p>
            {t("实际耗时（毫秒）")}：{selected.elapsed_ms ?? t("未知")} · {selected.reason}
          </p>
          <p>
            {t(
              "停止后不新增接纳；已接纳任务按原取消流程结束。不确定回执保持待核对，重启不自动续跑。",
            )}
          </p>
          <button
            disabled={blocked || retired || selected.phase !== "saved"}
            onClick={() => void start()}
          >
            {startId ? t("确认批次启动状态") : t("确认启动批次")}
          </button>
          <label>
            {t("停止原因")}
            <input
              disabled={blocked || !!stopId}
              value={reason}
              onChange={event => setReason(event.target.value)}
            />
          </label>
          <button
            disabled={
              blocked ||
              !reason.trim() ||
              !["saved", "running", "stopping"].includes(selected.phase)
            }
            onClick={() => void stop()}
          >
            {stopId ? t("确认批次停止状态") : t("停止批次")}
          </button>
          <button disabled={blocked} onClick={() => void read(selected.definition.object.id)}>
            {t("刷新批次状态")}
          </button>
          <table aria-label={t("批次全部成员")}>
            <thead>
              <tr>
                <th>{t("任务")}</th>
                <th>{t("滑点（跳）")}</th>
                <th>{t("手续费倍率（合成情景）")}</th>
                <th>{t("接纳状态")}</th>
                <th>{t("状态")}</th>
              </tr>
            </thead>
            <tbody>
              {selected.members.map(member => (
                <tr key={member.task_id}>
                  <td>{member.task_id}</td>
                  <td>
                    {
                      selected.definition.members.find(x => x.task_id === member.task_id)
                        ?.slippage_ticks
                    }
                  </td>
                  <td>
                    {selected.definition.members.find(x => x.task_id === member.task_id)
                      ?.fee_multiplier ?? "1"}
                  </td>
                  <td>
                    {t(`batch.admission.${member.admission}`)} · {member.error?.message}
                  </td>
                  <td>
                    {t(
                      states[reply?.members.find(x => x.id === member.task_id)?.state ?? ""] ??
                        "尚无已确认任务",
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <button disabled={blocked} onClick={() => void readResults()}>
            {t("读取成本敏感性结果")}
          </button>
          <p>{t("仅成功成员可读取收益；未完成、失败或未知项保持列示，不按部分结果排名。")}</p>
        </div>
      )}
      {!!results.length && (
        <>
          <p>
            {t(
              "成本假设敏感性：每个成员只改变所声明的滑点或手续费倍率，其他固定输入和风险实现一致。实际成交和资金路径可能变化，不能机械扣费归因；独立列示原结果，不排序、不宣称最优。",
            )}
          </p>
          <table aria-label={t("成本敏感性结果")}>
            <thead>
              <tr>
                <th>{t("任务")}</th>
                <th>{t("滑点（跳）")}</th>
                <th>{t("手续费倍率（合成情景）")}</th>
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
                  <td>
                    {selected?.definition.members.find(x => x.task_id === value.id)
                      ?.fee_multiplier ?? "1"}
                  </td>
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
