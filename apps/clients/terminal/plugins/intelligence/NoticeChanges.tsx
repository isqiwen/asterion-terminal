import { useEffect, useRef, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type {
  MaterialRead,
  NoticeChangeRecord,
  NoticeChangeRequest,
  NoticeEventView,
  NoticeObject,
  NoticeRecord,
  NoticeSpan,
} from "../../src/bridge/client";
import { NoticeConsumers } from "./NoticeConsumers";
const t = (key: string) => translate("asterion.terminal.intelligence", key);
const same = (a: NoticeObject, b: NoticeObject) =>
  (["node", "service", "domain", "kind", "id", "version"] as const).every(key => a[key] === b[key]);
const resume = (r: NoticeChangeRecord): NoticeChangeRequest => ({
  id: r.reference.id,
  event: r.event,
  expected_head: r.expected_head,
  action: r.action,
  replacement: r.replacement,
  material: r.material,
  evidence: r.evidence,
  operator_name: r.operator_name,
  reason: r.reason,
  human_confirmed: true,
  effective_at_ms: r.effective_at_ms,
});
export function NoticeChanges({
  context,
  record,
}: {
  context: TerminalContext;
  record: NoticeRecord;
}) {
  const [view, setView] = useState<NoticeEventView | null>(null);
  const [cutoff, setCutoff] = useState("");
  const [observedCutoff, setObservedCutoff] = useState<string | null>(null);
  const [materialId, setMaterialId] = useState("");
  const [material, setMaterial] = useState<MaterialRead | null>(null);
  const [quote, setQuote] = useState<NoticeSpan | null>(null);
  const [action, setAction] = useState<"correct" | "withdraw">("correct");
  const [operator, setOperator] = useState(""),
    [reason, setReason] = useState("");
  const [effectiveAt, setEffectiveAt] = useState("");
  const [human, setHuman] = useState(false);
  const [operationId, setOperationId] = useState<string>(() => crypto.randomUUID());
  const [attempt, setAttempt] = useState<NoticeChangeRequest | null>(null);
  const [pending, setPending] = useState(false),
    [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  const event = record.event;
  const key = JSON.stringify([event?.reference, record.published?.reference]);
  const ownerKey = context.snapshot?.intelligence_service?.connection_id;
  const dataKey = context.snapshot?.data?.connection_id;
  useEffect(() => {
    const token = epoch.current;
    ++token.value;
    setView(null);
    setAttempt(null);
    setMaterial(null);
    setQuote(null);
    setHuman(false);
    setOperationId(crypto.randomUUID());
    setPending(false);
    setError("");
    return () => {
      ++token.value;
    };
  }, [key, ownerKey, dataKey]);
  if (!event || !record.published) return null;
  const accept = (value: NoticeEventView | undefined, asOf: string | null) => {
    if (!value || !same(value.event.reference, event.reference))
      throw Error(t("事件响应不符，请重新读取"));
    setView(value);
    setObservedCutoff(asOf);
    setAttempt(value.pending_change ? resume(value.pending_change) : null);
  };
  async function run(work: (current: number) => Promise<void>) {
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    try {
      await work(current);
    } catch (failure) {
      if (current === epoch.current.value) setError(asDisplayError(failure));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  const inspect = () =>
    run(async current => {
      const asOf = cutoff.trim() || null;
      const reply = await context.query("intelligence.event.view", {
        event: event.reference,
        as_of_ms: asOf,
      });
      if (current === epoch.current.value) accept(reply.notice_event_view, asOf);
    });
  const submit = () =>
    run(async current => {
      const request = attempt ?? {
        id: operationId,
        event: event.reference,
        expected_head: view!.head,
        action,
        replacement: action === "correct" ? record.published!.reference : null,
        material: material!.receipt.reference,
        evidence: quote!,
        operator_name: operator,
        reason,
        human_confirmed: human,
        effective_at_ms: effectiveAt.trim() || null,
      };
      setAttempt(request);
      await context.trade("intelligence.change", { request });
      if (current !== epoch.current.value) return;
      const reply = await context.query("intelligence.event.view", {
        event: event.reference,
        as_of_ms: null,
      });
      if (current !== epoch.current.value) return;
      accept(reply.notice_event_view, null);
      setHuman(false);
      setOperationId(crypto.randomUUID());
    });
  const canSubmit =
    !!view &&
    observedCutoff === null &&
    (!!attempt ||
      (!view.withdrawn &&
        !!material?.text &&
        !!quote &&
        material.receipt.request.use_scope === event.use_scope &&
        !!operator.trim() &&
        !!reason.trim() &&
        human &&
        !!operationId &&
        (action !== "correct" || !same(record.published.reference, view.selected_publication))));
  return (
    <section aria-label={t("事件更正与撤回")}>
      <h3>{t("事件更正与撤回")}</h3>
      <p>{t("追加变化保留原记录，不改固定研究，不操作订单或持仓。")}</p>
      <p>
        {t("事件身份")}：
        <code>
          {event.reference.id} / {event.reference.version}
        </code>
      </p>
      <label>
        {t("截至时间（UTC 毫秒，留空查询当前）")}
        <input
          value={cutoff}
          onChange={e => {
            setCutoff(e.target.value);
            setView(null);
          }}
        />
      </label>
      <button disabled={pending} onClick={() => void inspect()}>
        {t("读取事件状态")}
      </button>
      {view && (
        <div>
          <p>
            {observedCutoff === null ? t("当前已知状态") : t("截至时点状态")}：
            {view.withdrawn ? t("已撤回") : t("可用陈述")}
          </p>
          <p>
            {t("原始发布")}：
            <code>
              {view.event.initial_publication.id} / {view.event.initial_publication.version}
            </code>
          </p>
          <p>
            {t("当前变化头")}：
            <code>
              {view.head.id} / {view.head.version}
            </code>
          </p>
          <p>
            {t("所选陈述")}：
            <code>
              {view.selected_publication.id} / {view.selected_publication.version}
            </code>
          </p>
          {view.latest_change && (
            <div>
              <p>
                {t("变化理由")}：{view.latest_change.reason}
              </p>
              <p>
                {t("变化实际可用时间")}：{view.latest_change.published?.available_at_ms}
              </p>
              <p>
                {t("受影响原发布")}：<code>{view.latest_change.affected_publication.id}</code>
              </p>
              <NoticeConsumers context={context} source={view.latest_change.affected_publication} />
            </div>
          )}
        </div>
      )}
      {attempt && (
        <div>
          <p>
            {t("原变化待确认，按原操作续办")}：<code>{attempt.id}</code>
          </p>
          <p>
            {attempt.operator_name}：{attempt.reason}
          </p>
          <p>
            {t("当前变化头")}：<code>{attempt.expected_head.id}</code>
          </p>
          {attempt.replacement && (
            <p>
              {t("所选陈述")}：
              <code>
                {attempt.replacement.id} / {attempt.replacement.version}
              </code>
            </p>
          )}
          <blockquote>{attempt.evidence.text}</blockquote>
        </div>
      )}
      <fieldset disabled={pending || !!attempt}>
        <legend>{t("人工追加变化")}</legend>
        <label>
          {t("变化操作 ID")}
          <input value={operationId} onChange={e => setOperationId(e.target.value)} />
        </label>
        <label>
          {t("变化动作")}
          <select value={action} onChange={e => setAction(e.target.value as typeof action)}>
            <option value="correct">{t("更正为本页已发布陈述")}</option>
            <option value="withdraw">{t("撤回当前陈述")}</option>
          </select>
        </label>
        <label>
          {t("新证据材料 ID")}
          <input
            value={materialId}
            onChange={e => {
              setMaterialId(e.target.value);
              setMaterial(null);
              setQuote(null);
            }}
          />
        </label>
        <button
          disabled={!context.snapshot?.data?.online || !materialId}
          onClick={() =>
            void run(async current => {
              const reply = await context.query("data.material.read", { id: materialId });
              if (current !== epoch.current.value) return;
              if (!reply.material_read || reply.material_read.receipt.reference.id !== materialId)
                throw Error(t("固定材料响应缺失"));
              setMaterial(reply.material_read);
              setQuote(null);
            })
          }
        >
          {t("读取新证据")}
        </button>
        {material?.text && (
          <textarea
            aria-label={t("变化证据原文")}
            readOnly
            value={material.text}
            onSelect={e => {
              const field = e.currentTarget,
                text = field.value.substring(field.selectionStart, field.selectionEnd);
              const encode = new TextEncoder();
              const begin = encode.encode(field.value.substring(0, field.selectionStart)).length;
              setQuote(text ? { begin, end: begin + encode.encode(text).length, text } : null);
            }}
          />
        )}
        {quote && <blockquote>{quote.text}</blockquote>}
        <label>
          {t("核验人")}
          <input value={operator} onChange={e => setOperator(e.target.value)} />
        </label>
        <label>
          {t("变化理由")}
          <input value={reason} onChange={e => setReason(e.target.value)} />
        </label>
        <label>
          {t("业务生效时间（UTC 毫秒，未知留空）")}
          <input value={effectiveAt} onChange={e => setEffectiveAt(e.target.value)} />
        </label>
        <label>
          <input type="checkbox" checked={human} onChange={e => setHuman(e.target.checked)} />
          {t("已核对新证据、事件归属和受影响引用")}
        </label>
      </fieldset>
      <button disabled={pending || !canSubmit} onClick={() => void submit()}>
        {t(attempt ? "续办原变化发布" : "保存并追加变化")}
      </button>
      <p>{t("消费者列表是当前登记，订阅交付尚未实现。")}</p>
      <ErrorNotice error={error} />
    </section>
  );
}
