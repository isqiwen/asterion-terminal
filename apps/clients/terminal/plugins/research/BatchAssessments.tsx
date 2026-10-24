import { useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type ResourceObject,
  type ResourceEntry,
  type ResearchBatchAssessment,
  type ResearchBatchAssessmentSnapshot,
  type ResearchBatchAssessmentDeclaration,
  type TaskResult,
} from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { t } from "./shared";
import { BatchAssessmentExport } from "./BatchAssessmentExport";

type Source = Extract<TaskResult, { kind: "backtest" }>;
type Draft = Omit<
  ResearchBatchAssessmentDeclaration,
  "id" | "batch" | "expected_snapshot" | "judgment"
> & {
  judgment: ResearchBatchAssessmentDeclaration["judgment"] | "";
};
const objectKey = (object: ResourceObject) =>
  JSON.stringify([
    object.node,
    object.service,
    object.domain,
    object.kind,
    object.id,
    object.version,
  ]);
const states = {
  queued: "排队中",
  running: "运行中",
  cancel_requested: "正在取消",
  publishing: "正在发布",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};
const admissions = {
  waiting: "未接纳",
  pending: "接纳回执不确定",
  admitted: "已接纳",
  rejected: "接纳已拒绝",
};
const judgments = { supported: "支持此范围", refuted: "不支持此范围", insufficient: "证据不足" };

export function BatchAssessments({
  snapshot,
  busy,
  query,
  trade,
}: Pick<TerminalContext, "snapshot" | "busy" | "query" | "trade">) {
  const service = snapshot?.research_service;
  const ownerKey = JSON.stringify([
    service?.connection_id,
    service?.owner_node_id,
    service?.service,
  ]);
  const epoch = useRef({ value: 0 });
  const [batches, setBatches] = useState<ResourceEntry[]>([]);
  const [batchCursor, setBatchCursor] = useState("");
  const [batch, setBatch] = useState<ResourceObject | null>(null);
  const [evidence, setEvidence] = useState<{
    snapshot: ResearchBatchAssessmentSnapshot;
    digest: string;
    source: Source;
  } | null>(null);
  const [fresh, setFresh] = useState(false);
  const [saved, setSaved] = useState<ResearchBatchAssessment | null>(null);
  const [reviews, setReviews] = useState<ResearchBatchAssessment[]>([]);
  const [reviewCursor, setReviewCursor] = useState("");
  const [reviewId, setReviewId] = useState("");
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError | "">("");
  const [confirmed, setConfirmed] = useState(false);
  const key = JSON.stringify([ownerKey, batch && objectKey(batch)]);
  const [draft, setDraft] = useWorkspaceDraft<Draft>(`batch-assessment:${key}`, {
    question: "",
    scope: "",
    judgment: "",
    reason: "",
    selected_task_id: "",
    selection_reason: "",
    unselected_reason: "",
    validation_information_used: false,
    information_use: "",
    previous_id: "",
    revision_reason: "",
  });
  const fingerprint = JSON.stringify([evidence?.digest, draft]);
  const [operation, setOperation] = useWorkspaceRequestId(
    `batch-assessment-save:${key}`,
    fingerprint,
  );
  useWorkspaceUnsaved(
    `batch-assessment:${key}`,
    Boolean(batch && draft.question.trim() && !operation),
  );
  useEffect(() => {
    const token = epoch.current;
    ++token.value;
    setBatches([]);
    setBatchCursor("");
    setBatch(null);
    setEvidence(null);
    setSaved(null);
    setReviews([]);
    setReviewId("");
    setReviewCursor("");
    setFresh(false);
    setConfirmed(false);
    setError("");
    setPending(false);
    return () => {
      ++token.value;
    };
  }, [ownerKey]);
  useEffect(() => {
    setConfirmed(false);
  }, [fingerprint]);
  const blocked = busy || pending || !service?.online;
  const owned = (object: ResourceObject) =>
    object.node === service?.owner_node_id &&
    object.service === service?.service &&
    object.domain === "research";
  async function original(observed: ResearchBatchAssessmentSnapshot): Promise<Source> {
    const reply = await query("task.result.read", {
      owner: taskOwner(service, "research"),
      id: observed.source.id,
    });
    const source = reply.task_result;
    if (
      source?.kind !== "backtest" ||
      source.id !== observed.source.id ||
      source.input_sha256 !== observed.source.input_sha256 ||
      source.task.result_digest !== observed.source.result_sha256 ||
      source.task.program_artifact !== observed.source.program_artifact ||
      source.task.risk_artifact !== observed.source.risk_artifact
    )
      throw new Error(t("原基准条件身份不符，请重新读取"));
    return source;
  }
  async function loadBatches(after = "") {
    if (blocked) return;
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    try {
      const reply = await query("resource.catalogue", {
        domain: "research",
        kind: "backtest_batch",
        after,
      });
      if (current !== epoch.current.value) return;
      const page = reply.resources;
      if (
        !page ||
        page.kind !== "backtest_batch" ||
        page.owner.node !== service?.owner_node_id ||
        page.owner.service !== service?.service ||
        page.owner.domain !== "research"
      )
        throw new Error(t("批次目录所属者不符，请重新读取"));
      setBatches(previous => (after ? [...previous, ...page.items] : page.items));
      setBatchCursor(page.next_after);
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  async function preview() {
    if (blocked || !batch) return;
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    setFresh(false);
    setConfirmed(false);
    try {
      const reply = (await query("research.batch.assessment.preview", { batch }))
        .research_assessment;
      if (current !== epoch.current.value) return;
      if (
        !reply?.preview ||
        !reply.preview_digest ||
        objectKey(reply.preview.batch.definition.object) !== objectKey(batch)
      )
        throw new Error(t("批次预览身份不符，请重新读取"));
      const source = await original(reply.preview);
      if (current !== epoch.current.value) return;
      setEvidence({ snapshot: reply.preview, digest: reply.preview_digest, source });
      setSaved(null);
      setFresh(true);
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  const valid = Boolean(
    batch &&
    evidence &&
    fresh &&
    confirmed &&
    draft.judgment &&
    draft.question.trim() &&
    draft.scope.trim() &&
    draft.reason.trim() &&
    draft.selection_reason.trim() &&
    draft.unselected_reason.trim() &&
    draft.information_use.trim() &&
    Boolean(draft.previous_id.trim()) === Boolean(draft.revision_reason.trim()),
  );
  async function save() {
    if (blocked || !valid || !batch || !evidence || !draft.judgment) return;
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    const id = operation ?? `assessment-${crypto.randomUUID()}`;
    setOperation(id);
    const definition: ResearchBatchAssessmentDeclaration = {
      ...draft,
      judgment: draft.judgment,
      id,
      batch,
      expected_snapshot: evidence.digest,
    };
    try {
      await trade("research.batch.assessment.save", { id, definition });
      if (current !== epoch.current.value) return;
      const value = (await query("research.batch.assessment.read", { id })).research_assessment
        ?.assessment;
      if (current !== epoch.current.value) return;
      if (
        !value ||
        !owned(value.object) ||
        value.object.id !== id ||
        value.declaration.expected_snapshot !== evidence.digest
      )
        throw new Error(t("固定审阅回执身份不符，请检查原请求"));
      setSaved(value);
      setFresh(false);
      setConfirmed(false);
      setReviewId(value.object.id);
      setReviews(previous => [
        ...previous.filter(item => item.object.id !== value.object.id),
        value,
      ]);
    } catch (reason) {
      if (current === epoch.current.value) {
        setError(asDisplayError(reason));
        setConfirmed(false);
      }
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  async function loadReviews(after = "") {
    if (blocked || !batch) return;
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    try {
      const reply = (
        await query("research.batch.assessment.list", { batch_id: batch.id, after, limit: 20 })
      ).research_assessment;
      if (current !== epoch.current.value) return;
      if (
        !reply ||
        reply.assessments.some(
          item => !owned(item.object) || objectKey(item.declaration.batch) !== objectKey(batch),
        )
      )
        throw new Error(t("固定审阅列表所属者不符，请重新读取"));
      setReviews(previous => (after ? [...previous, ...reply.assessments] : reply.assessments));
      setReviewCursor(reply.next_after);
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  async function read() {
    if (blocked || !reviewId || !batch) return;
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    setFresh(false);
    setConfirmed(false);
    try {
      const value = (await query("research.batch.assessment.read", { id: reviewId }))
        .research_assessment?.assessment;
      if (current !== epoch.current.value) return;
      if (
        !value ||
        !owned(value.object) ||
        value.object.id !== reviewId ||
        objectKey(value.declaration.batch) !== objectKey(batch)
      )
        throw new Error(t("固定审阅读取身份不符，请重新读取"));
      const source = await original(value.snapshot);
      if (current !== epoch.current.value) return;
      setEvidence({
        snapshot: value.snapshot,
        digest: value.declaration.expected_snapshot,
        source,
      });
      setSaved(value);
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  const choose = (id: string) => {
    ++epoch.current.value;
    setBatch(batches.find(item => item.object.id === id)?.object ?? null);
    setEvidence(null);
    setSaved(null);
    setReviews([]);
    setReviewId("");
    setReviewCursor("");
    setFresh(false);
    setConfirmed(false);
    setError("");
  };
  const candidates = evidence
    ? [evidence.snapshot.source, ...evidence.snapshot.members].filter(
        item => item.state === "succeeded",
      )
    : [];
  return (
    <section aria-label={t("有限批次稳健性审阅")}>
      <h3>{t("有限批次稳健性审阅")}</h3>
      <p>
        {t(
          "读取整个声明批次，包括失败、取消、未接纳及不确定成员；不启动、补跑、排序或自动选赢家。",
        )}
      </p>
      <p>
        {t("此处保存事后人工判断。声明未使用验证信息不证明独立性；原完整账户不是独立留出结论。")}
      </p>
      {error && <ErrorNotice error={error} />}
      <fieldset disabled={blocked}>
        <button onClick={() => void loadBatches()}>{t("读取可审阅批次目录")}</button>
        {batchCursor && (
          <button onClick={() => void loadBatches(batchCursor)}>{t("更多可审阅批次")}</button>
        )}
        <label>
          {t("完整批次来源")}
          <select value={batch?.id ?? ""} onChange={event => choose(event.target.value)}>
            <option value="">{t("选择固定批次")}</option>
            {batch && !batches.some(item => item.object.id === batch.id) && (
              <option value={batch.id}>{batch.id}</option>
            )}
            {batches.map(item => (
              <option key={objectKey(item.object)} value={item.object.id}>
                {item.object.id} · {item.name}
              </option>
            ))}
          </select>
        </label>
        <button disabled={!batch} onClick={() => void preview()}>
          {t("读取完整批次预览")}
        </button>
        <button disabled={!batch} onClick={() => void loadReviews()}>
          {t("读取该批次已保存审阅")}
        </button>
        <label>
          {t("已保存批次审阅")}
          <select value={reviewId} onChange={event => setReviewId(event.target.value)}>
            <option value="">{t("选择固定审阅")}</option>
            {reviews.map(item => (
              <option key={item.object.version} value={item.object.id}>
                {item.object.id} · {item.declaration.question}
              </option>
            ))}
          </select>
        </label>
        <button disabled={!reviewId} onClick={() => void read()}>
          {t("读取固定批次审阅")}
        </button>
        {reviewCursor && (
          <button onClick={() => void loadReviews(reviewCursor)}>{t("更多固定审阅")}</button>
        )}
      </fieldset>
      {evidence && (
        <>
          <p>
            {saved
              ? t("当前显示已保存原审阅；新判断须重新预览并明确保存。")
              : t("预览尚未保存；保存时会再次核对全部成员状态，变化后须重新读取。")}
          </p>
          <BatchAssessmentEvidence
            snapshot={saved?.snapshot ?? evidence.snapshot}
            source={evidence.source}
          />
          {saved && (
            <article aria-label={t("已保存固定批次审阅")}>
              <h4>{saved.declaration.question}</h4>
              <code>
                {saved.object.id} · {saved.object.version}
              </code>
              <dl>
                <dt>{t("明确人工判断")}</dt>
                <dd>{t(judgments[saved.declaration.judgment])}</dd>
                <dt>{t("人工判断理由")}</dt>
                <dd>{saved.declaration.reason}</dd>
                <dt>{t("审阅范围")}</dt>
                <dd>{saved.declaration.scope}</dd>
                <dt>{t("被选原任务")}</dt>
                <dd>{saved.declaration.selected_task_id || t("不选候选")}</dd>
                <dt>{t("选择或不选择理由")}</dt>
                <dd>{saved.declaration.selection_reason}</dd>
                <dt>{t("未选成员及不利证据说明")}</dt>
                <dd>{saved.declaration.unselected_reason}</dd>
                <dt>{t("验证信息使用声明")}</dt>
                <dd>
                  {t(
                    saved.declaration.validation_information_used
                      ? "已使用验证信息"
                      : "声明未使用；独立性仍未建立",
                  )}
                </dd>
                <dt>{t("所见信息与用途")}</dt>
                <dd>{saved.declaration.information_use}</dd>
                <dt>{t("前次判断")}</dt>
                <dd>{saved.declaration.previous_id || t("无前次判断")}</dd>
                <dt>{t("追加修订理由")}</dt>
                <dd>{saved.declaration.revision_reason || t("未追加修订")}</dd>
                <dt>{t("实际记录程序")}</dt>
                <dd>
                  <code>{saved.program_artifact}</code>
                </dd>
                <dt>{t("保存时刻（UTC毫秒）")}</dt>
                <dd>{saved.recorded_at_ms}</dd>
              </dl>
              <button
                disabled={blocked}
                onClick={() =>
                  setDraft({ ...draft, previous_id: saved.object.id, revision_reason: "" })
                }
              >
                {t("将此审阅引用为前次判断")}
              </button>
              <BatchAssessmentExport assessment={saved} query={query} busy={blocked} />
            </article>
          )}
          <fieldset disabled={blocked || !fresh}>
            <legend>{t("明确保存全批次人工判断")}</legend>
            {(
              [
                ["question", "审阅问题"],
                ["scope", "审阅范围"],
                ["reason", "人工判断理由"],
                ["selection_reason", "选择或不选择理由"],
                ["unselected_reason", "未选成员及不利证据说明"],
                ["information_use", "所见信息与用途"],
                ["previous_id", "前次判断ID（可空）"],
                ["revision_reason", "追加修订理由"],
              ] as const
            ).map(([field, label]) => (
              <label key={field}>
                {t(label)}
                <input
                  maxLength={4096}
                  value={draft[field]}
                  onChange={event => setDraft({ ...draft, [field]: event.target.value })}
                />
              </label>
            ))}
            <label>
              {t("明确人工判断")}
              <select
                value={draft.judgment}
                onChange={event =>
                  setDraft({ ...draft, judgment: event.target.value as Draft["judgment"] })
                }
              >
                <option value="">{t("请选择人工判断")}</option>
                {(Object.keys(judgments) as (keyof typeof judgments)[]).map(value => (
                  <option key={value} value={value}>
                    {t(judgments[value])}
                  </option>
                ))}
              </select>
            </label>
            <label>
              {t("被选原任务")}
              <select
                value={draft.selected_task_id}
                onChange={event => setDraft({ ...draft, selected_task_id: event.target.value })}
              >
                <option value="">{t("不选候选")}</option>
                {candidates.map(item => (
                  <option key={item.id} value={item.id}>
                    {item.id}
                  </option>
                ))}
              </select>
            </label>
            <label>
              <input
                type="checkbox"
                checked={draft.validation_information_used}
                onChange={event =>
                  setDraft({ ...draft, validation_information_used: event.target.checked })
                }
              />
              {t("已使用验证信息影响判断或选择")}
            </label>
            <label>
              <input
                type="checkbox"
                checked={confirmed}
                onChange={event => setConfirmed(event.target.checked)}
              />
              {t("已核对全批次、缺失与不利证据，明确保存人工判断；不授予交易权限")}
            </label>
            <button disabled={!valid} onClick={() => void save()}>
              {t(operation ? "重试同一审阅请求" : "保存固定批次审阅")}
            </button>
          </fieldset>
          {operation && (
            <p>
              {t("结果不确定时先检查原请求，不自动重发或补跑成员。")} <code>{operation}</code>
            </p>
          )}
        </>
      )}
    </section>
  );
}

export function BatchAssessmentEvidence({
  snapshot,
  source,
}: {
  snapshot: ResearchBatchAssessmentSnapshot;
  source: Source;
}) {
  const rows = new Map(snapshot.members.map(task => [task.id, task]));
  return (
    <section aria-label={t("全批次固定证据")}>
      <h4>{snapshot.batch.definition.name}</h4>
      <p>
        {snapshot.batch.definition.purpose} · {snapshot.batch.definition.limitations}
      </p>
      <p>
        {t("原批次阶段与停止原因")}：{snapshot.batch.phase} ·{" "}
        {snapshot.batch.reason || t("未记录停止原因")}
      </p>
      <p>
        {t("原基准程序")}：<code>{snapshot.source.program_artifact}</code> · {t("原风险实现")}：
        <code>{snapshot.source.risk_artifact}</code>
      </p>
      <p>
        {t("原基准固定输入")}：<code>{snapshot.source.input_sha256}</code> · {t("原基准固定结果")}：
        <code>{snapshot.source.result_sha256}</code>
      </p>
      <p>
        {t("情景只改变原合约滑点tick；费用金额另列，不把损益差一概归因佣金。未取得成功结果不补0。")}
      </p>
      <table aria-label={t("全部声明成员与原账户金额")}>
        <thead>
          <tr>
            {[
              "角色或声明滑点tick",
              "原任务",
              "接纳事实",
              "执行状态",
              "尝试",
              "原完整账户净损益",
              "原记账费用",
              "原错误或缺失原因",
            ].map(label => (
              <th key={label}>{t(label)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          <tr>
            <td>{t("原基准（各合约原tick见下表）")}</td>
            <td>{snapshot.source.id}</td>
            <td>{t("原已发布结果")}</td>
            <td>{t(states[snapshot.source.state])}</td>
            <td>{snapshot.source.attempt}</td>
            <td>
              {snapshot.source.account?.net_profit} {snapshot.source.account?.currency}
            </td>
            <td>
              {snapshot.source.account?.fees} {snapshot.source.account?.currency}
            </td>
            <td>{snapshot.source.error}</td>
          </tr>
          {snapshot.batch.definition.members.map(member => {
            const task = rows.get(member.task_id);
            const receipt = snapshot.batch.members.find(value => value.task_id === member.task_id);
            const problem = task?.error || receipt?.error?.message;
            return (
              <tr key={member.task_id}>
                <td>
                  {member.slippage_ticks} {t("tick（原变动价位）")}
                  {member.fee_multiplier !== undefined && (
                    <>
                      {" "}
                      · {t("手续费倍率（合成情景）")}：{member.fee_multiplier}
                    </>
                  )}
                </td>
                <td>{member.task_id}</td>
                <td>{receipt ? t(admissions[receipt.admission]) : t("接纳记录缺失")}</td>
                <td>{task ? t(states[task.state]) : t("没有可确认的原任务")}</td>
                <td>{task?.attempt ?? t("未接纳")}</td>
                <td>
                  {task?.account
                    ? `${task.account.net_profit} ${task.account.currency}`
                    : t("未取得成功原结果")}
                </td>
                <td>
                  {task?.account
                    ? `${task.account.fees} ${task.account.currency}`
                    : t("未取得成功原结果")}
                </td>
                <td>{problem && <ErrorNotice error={asDisplayError(problem)} />}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <details>
        <summary>{t("原基准费用、保证金及合约单位（固定输入）")}</summary>
        <table>
          <thead>
            <tr>
              {[
                "原合约",
                "币种",
                "价格变动单位",
                "合约乘数",
                "原滑点tick",
                "原费用与保证金日程",
              ].map(label => (
                <th key={label}>{t(label)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {source.experiment.paper.contracts.map((contract, index) => (
              <tr key={index}>
                <td>
                  {contract.dataset.contract.venue}/{contract.dataset.contract.symbol}
                </td>
                <td>{contract.dataset.contract.currency}</td>
                <td>{contract.dataset.contract.price_increment}</td>
                <td>{contract.dataset.contract.multiplier}</td>
                <td>{contract.slippage_ticks}</td>
                <td>
                  <pre>{JSON.stringify(contract.cost_schedule, null, 2)}</pre>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <p>{t("原风险限额（固定输入，不修改政策）")}</p>
        <pre>{JSON.stringify(source.experiment.paper.risk, null, 2)}</pre>
      </details>
      <p>
        {t("范围为原完整账户；缺点、失败及不确定接纳仍保留，不以相同数据或成功个数证明独立性。")}
      </p>
    </section>
  );
}
