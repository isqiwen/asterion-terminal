import { useEffect, useRef, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  useWorkspaceDraft,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type {
  NoticeObject,
  NoticeDeliveryKind,
  NoticeDeliveryFilter,
  NoticeDeliveryPage,
  NoticeSubscription,
  NoticeSubscriptionDefinition,
  NoticeSubscriptionOffer,
  NoticeDeliveryBatch,
  NoticeReminderPage,
  NoticeRecord,
  NoticeChangeRecord,
} from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.intelligence", key);
const kinds: NoticeDeliveryKind[] = ["publication", "correction", "withdrawal"];
const kindNames = { publication: "事件首次发布", correction: "事件更正", withdrawal: "事件撤回" };
const same = (a: NoticeObject, b: NoticeObject) =>
  (["node", "service", "domain", "kind", "id", "version"] as const).every(k => a[k] === b[k]);
const sameFilter = (a: NoticeDeliveryFilter, b: NoticeDeliveryFilter) =>
  a.rule_id === b.rule_id &&
  a.use_scope === b.use_scope &&
  a.include_followup_statements === b.include_followup_statements &&
  JSON.stringify(a.contracts) === JSON.stringify(b.contracts) &&
  JSON.stringify(a.kinds) === JSON.stringify(b.kinds);
const sameDefinition = (a: NoticeSubscriptionDefinition, b: NoticeSubscriptionDefinition) =>
  a.id === b.id &&
  a.name === b.name &&
  a.consumer === b.consumer &&
  a.start_after === b.start_after &&
  sameFilter(a.filter, b.filter);
function position(value: string) {
  if (!/^\d{1,19}$/.test(value)) return null;
  const parsed = BigInt(value);
  return parsed <= 9223372036854775807n ? parsed.toString() : null;
}
type Draft = {
  id: string;
  name: string;
  contracts: string;
  scope: string;
  start: string;
  kinds: NoticeDeliveryKind[];
  followup: boolean;
  through: string;
  operation: string;
  historical: boolean;
  creating: NoticeSubscriptionDefinition | null;
  offer: NoticeSubscriptionOffer | null;
};
export function NoticeSubscriptions({ context }: { context: TerminalContext }) {
  const owner = context.snapshot?.intelligence_service;
  const [draft, setDraft] = useWorkspaceDraft<Draft>(
    `notice-subscription:${owner?.owner_node_id ?? ""}:${owner?.service ?? ""}`,
    () => ({
      id: "",
      name: "",
      contracts: "",
      scope: "",
      start: "",
      kinds: [],
      followup: false,
      through: "",
      operation: "",
      historical: false,
      creating: null,
      offer: null,
    }),
  );
  const [preview, setPreview] = useState<{
    definition: NoticeSubscriptionDefinition;
    page: NoticeDeliveryPage;
  } | null>(null);
  const [view, setView] = useState<NoticeSubscription | null>(null);
  const [saved, setSaved] = useState<NoticeSubscription[] | null>(null);
  const [batch, setBatch] = useState<NoticeDeliveryBatch | null>(null);
  const [inbox, setInbox] = useState<NoticeReminderPage | null>(null);
  const [evidence, setEvidence] = useState<{
    source: NoticeObject;
    record?: NoticeRecord;
    change?: NoticeChangeRecord;
  } | null>(null);
  const [checked, setChecked] = useState(false);
  const [pending, setPending] = useState(false);
  const [uncertain, setUncertain] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  useEffect(() => {
    const token = epoch.current;
    ++token.value;
    setPreview(null);
    setView(null);
    setSaved(null);
    setBatch(null);
    setInbox(null);
    setEvidence(null);
    setChecked(false);
    setError("");
    setPending(false);
    setUncertain(false);
    return () => {
      ++token.value;
    };
  }, [owner?.connection_id]);
  const own = (ref: NoticeObject) =>
    ref.domain === "intelligence" &&
    ref.node === owner?.owner_node_id &&
    ref.service === owner?.service;
  const locked = pending || context.busy;
  const fixed = locked || !!draft.creating || !!draft.offer;
  const contracts = draft.contracts
    .split(/[,\n]/)
    .map(v => v.trim())
    .filter(Boolean)
    .sort();
  const start = position(draft.start);
  const through = position(draft.through);
  const valid =
    !!owner &&
    !!draft.id &&
    !!draft.name.trim() &&
    !!draft.scope.trim() &&
    start !== null &&
    contracts.length > 0 &&
    contracts.length <= 16 &&
    new Set(contracts).size === contracts.length &&
    draft.kinds.length > 0;
  const definition: NoticeSubscriptionDefinition | null = valid
    ? {
        id: draft.id,
        name: draft.name,
        filter: {
          contracts,
          rule_id: "contract_margin_announcement",
          use_scope: draft.scope,
          kinds: kinds.filter(k => draft.kinds.includes(k)),
          include_followup_statements: draft.followup,
        },
        start_after: start!,
        consumer: "application_reminder",
      }
    : null;
  function edit<K extends keyof Draft>(key: K, value: Draft[K]) {
    setDraft(prev => ({ ...prev, [key]: value }));
    setPreview(null);
    if (key === "id") {
      ++epoch.current.value;
      setView(null);
      setBatch(null);
      setInbox(null);
      setEvidence(null);
      setUncertain(false);
      setError("");
    }
  }
  async function run(work: (token: number) => Promise<void>) {
    const token = ++epoch.current.value;
    setPending(true);
    setError("");
    try {
      await work(token);
    } catch (reason) {
      if (token === epoch.current.value) {
        setError(asDisplayError(reason));
        setUncertain(true);
      }
    } finally {
      if (token === epoch.current.value) setPending(false);
    }
  }
  async function loadBatch(
    token: number,
    subscription: NoticeObject,
    operation: string,
    expected?: NoticeSubscriptionOffer,
    pendingRef?: NoticeObject | null,
  ) {
    const reply = await context.query("intelligence.subscription.batch", {
      subscription,
      operation_id: operation,
    });
    if (token !== epoch.current.value) return;
    const value = reply.notice_delivery_batch;
    if (
      !value ||
      !same(value.subscription, subscription) ||
      value.operation_id !== operation ||
      (pendingRef && !same(value.reference, pendingRef)) ||
      (expected &&
        (value.after !== expected.after ||
          value.through !== expected.through ||
          value.historical !== expected.historical))
    )
      throw Error(t("原交付批次响应不符，请核实原操作"));
    setBatch(value);
    setChecked(false);
    if (value.acknowledged)
      setDraft(prev => ({
        ...prev,
        offer: prev.offer?.operation_id === operation ? null : prev.offer,
      }));
    return value;
  }
  async function refresh(
    token: number,
    reconcile = false,
    original?: NoticeSubscriptionOffer,
    expectedDefinition = draft.creating,
  ) {
    let reply = await context.query("intelligence.subscription.get", { id: draft.id });
    if (token !== epoch.current.value) return;
    let value = reply.notice_subscription;
    if (!value || value.reference.id !== draft.id || !own(value.reference))
      throw Error(t("订阅响应不符，请重新读取"));
    if (reconcile && BigInt(value.head) > BigInt(value.confirmed_through)) {
      await context.trade("intelligence.subscription.control", {
        subscription: value.reference,
        revision: value.revision,
        active: value.active,
        gap_through: value.head,
      });
      if (token !== epoch.current.value) return;
      reply = await context.query("intelligence.subscription.get", { id: draft.id });
      if (token !== epoch.current.value) return;
      const current = reply.notice_subscription;
      if (!current || !same(current.reference, value.reference))
        throw Error(t("订阅响应不符，请重新读取"));
      value = current;
    }
    setView(value);
    setUncertain(false);
    if (expectedDefinition) {
      if (!sameDefinition(value.definition, expectedDefinition))
        throw Error(t("该订阅ID已保存不同条件，请读取原定义"));
      setDraft(prev => ({ ...prev, creating: null }));
    }
    const requested = original ?? draft.offer;
    if (requested && same(requested.subscription, value.reference))
      await loadBatch(token, value.reference, requested.operation_id, requested);
    else if (value.pending_delivery)
      await loadBatch(
        token,
        value.reference,
        value.pending_delivery.id,
        undefined,
        value.pending_delivery,
      );
    else {
      setBatch(null);
      setChecked(false);
    }
    if (token !== epoch.current.value) return;
    await loadInbox(token, value.reference, "0");
  }
  async function loadInbox(token: number, subscription: NoticeObject, after: string) {
    const reply = await context.query("intelligence.subscription.reminders", {
      subscription,
      after,
    });
    if (token !== epoch.current.value) return;
    if (!reply.notice_reminders || !same(reply.notice_reminders.subscription, subscription))
      throw Error(t("提醒记录响应不符，请重新读取"));
    setInbox(reply.notice_reminders);
  }
  async function control(active: boolean) {
    if (!view) return;
    await run(async token => {
      await context.trade("intelligence.subscription.control", {
        subscription: view.reference,
        revision: view.revision,
        active,
        gap_through: null,
      });
      if (token === epoch.current.value) await refresh(token);
    });
  }
  async function offer() {
    const request =
      draft.offer ??
      (view && through !== null
        ? {
            subscription: view.reference,
            operation_id: draft.operation,
            after: view.confirmed_through,
            through,
            historical: draft.historical,
          }
        : null);
    if (!request) return;
    await run(async token => {
      setDraft(prev => ({ ...prev, offer: request }));
      await context.trade("intelligence.subscription.offer", { request });
      if (token === epoch.current.value) await refresh(token, false, request);
    });
  }
  async function acknowledge() {
    if (!batch || batch.acknowledged || !checked) return;
    const original = {
      subscription: batch.subscription,
      operation_id: batch.operation_id,
      after: batch.after,
      through: batch.through,
      historical: batch.historical,
    };
    await run(async token => {
      await context.trade("intelligence.subscription.ack", {
        reference: batch.reference,
        subscription: batch.subscription,
      });
      if (token === epoch.current.value) await refresh(token, false, original);
    });
  }
  const gap = !!view && BigInt(view.gap_through) > BigInt(view.confirmed_through);
  const validOffer =
    !!view &&
    view.active &&
    !view.pending_delivery &&
    through !== null &&
    BigInt(through) >= BigInt(view.confirmed_through) &&
    BigInt(through) <= BigInt(view.head) &&
    !!draft.operation &&
    (!batch || batch.operation_id !== draft.operation) &&
    (!gap || draft.historical) &&
    !uncertain;
  return (
    <section className="research-workspace" aria-label={t("情报订阅与交付")}>
      <h2>{t("情报订阅与交付")}</h2>
      <p>
        {t("应用内提醒只交付已发布变化；交付确认不等于Research任务接纳、严格历史资格或交易执行。")}
      </p>
      <p>
        <code>
          {owner?.owner_node_id}/{owner?.service}
        </code>
      </p>
      <button
        disabled={locked}
        onClick={() =>
          void run(async () => {
            await context.trade("node.intelligence.local.open");
          })
        }
      >
        {t("打开本机情报服务")}
      </button>
      <p>{t("先连接情报所有者，再预览明确条件；更改条件需新订阅ID，不自动回填全部历史。")}</p>
      <button
        disabled={locked || !owner}
        onClick={() =>
          void run(async token => {
            const reply = await context.query("intelligence.subscription.list", {});
            if (token !== epoch.current.value) return;
            const page = reply.notice_subscriptions;
            if (
              !page ||
              page.owner.node !== owner?.owner_node_id ||
              page.owner.service !== owner?.service
            )
              throw Error(t("订阅列表来源不符"));
            setSaved(page.items);
          })
        }
      >
        {t("读取已保存订阅")}
      </button>
      {saved && (
        <section aria-label={t("已保存订阅")}>
          <p>{t("显示原所有者的已保存条件与状态；读取不恢复暂停、不确认缺口、不接纳研究任务。")}</p>
          {saved.length === 0 && <p>{t("当前所有者尚无已保存订阅")}</p>}
          <ul>
            {saved.map(item => (
              <li key={item.reference.id}>
                <p>
                  {item.definition.name} · <code>{item.reference.id}</code> ·{" "}
                  {t(item.active ? "订阅已启用" : "订阅已暂停")}
                </p>
                <p>
                  {t("最后确认位置")}：{item.confirmed_through} · {t("明确待补区间")}：
                  {item.gap_through}
                </p>
                <button
                  disabled={fixed}
                  onClick={() =>
                    void run(async token => {
                      const reply = await context.query("intelligence.subscription.get", {
                        id: item.reference.id,
                      });
                      if (token !== epoch.current.value) return;
                      const current = reply.notice_subscription;
                      if (!current || !same(current.reference, item.reference))
                        throw Error(t("订阅响应不符，请重新读取"));
                      const def = current.definition;
                      setDraft({
                        id: def.id,
                        name: def.name,
                        contracts: def.filter.contracts.join("\n"),
                        scope: def.filter.use_scope,
                        start: def.start_after,
                        kinds: def.filter.kinds,
                        followup: def.filter.include_followup_statements,
                        through: "",
                        operation: "",
                        historical: false,
                        creating: null,
                        offer: null,
                      });
                      setPreview(null);
                      setBatch(null);
                      setChecked(false);
                      setEvidence(null);
                      setView(current);
                      setUncertain(false);
                      if (current.pending_delivery)
                        await loadBatch(
                          token,
                          current.reference,
                          current.pending_delivery.id,
                          undefined,
                          current.pending_delivery,
                        );
                      if (token === epoch.current.value)
                        await loadInbox(token, current.reference, "0");
                    })
                  }
                >
                  {t("读取此订阅原条件")}
                </button>
              </li>
            ))}
          </ul>
        </section>
      )}
      <label>
        {t("订阅 ID")}
        <input value={draft.id} disabled={fixed} onChange={e => edit("id", e.target.value)} />
      </label>
      <button
        disabled={fixed}
        onClick={() => {
          setDraft(prev => ({
            ...prev,
            id: crypto.randomUUID(),
            creating: null,
            offer: null,
            operation: "",
          }));
          setPreview(null);
          setView(null);
          setBatch(null);
          setInbox(null);
          setEvidence(null);
          setError("");
          setUncertain(false);
        }}
      >
        {t("新建订阅 ID")}
      </button>
      <label>
        {t("订阅名称")}
        <input
          value={draft.name}
          disabled={fixed || !!view}
          onChange={e => edit("name", e.target.value)}
        />
      </label>
      <label>
        {t("明确合约清单（每行一个）")}
        <textarea
          value={draft.contracts}
          disabled={fixed || !!view}
          onChange={e => edit("contracts", e.target.value)}
        />
      </label>
      <label>
        {t("使用范围")}
        <input
          value={draft.scope}
          disabled={fixed || !!view}
          onChange={e => edit("scope", e.target.value)}
        />
      </label>
      <label>
        {t("起点位置（之后，不含该位置）")}
        <input
          value={draft.start}
          inputMode="numeric"
          disabled={fixed || !!view}
          onChange={e => edit("start", e.target.value)}
        />
      </label>
      <fieldset disabled={fixed || !!view}>
        <legend>{t("变化范围")}</legend>
        {kinds.map(kind => (
          <label key={kind}>
            <input
              type="checkbox"
              checked={draft.kinds.includes(kind)}
              onChange={e =>
                edit(
                  "kinds",
                  e.target.checked ? [...draft.kinds, kind] : draft.kinds.filter(v => v !== kind),
                )
              }
            />
            {t(kindNames[kind])}
          </label>
        ))}
        <label>
          <input
            type="checkbox"
            checked={draft.followup}
            onChange={e => edit("followup", e.target.checked)}
          />
          {t("另含未应用为事件头的后续陈述发布")}
        </label>
      </fieldset>
      <p>{t("当前仅支持人工保证金事件、应用内消费者和有界拉取；不提供外部推送或策略自动消费。")}</p>
      <button
        disabled={fixed || !!view || !definition}
        onClick={() =>
          void run(async token => {
            const reply = await context.query("intelligence.subscription.preview", {
              definition: definition!,
            });
            if (token !== epoch.current.value) return;
            const page = reply.notice_delivery_page;
            if (
              !page ||
              page.owner.node !== owner?.owner_node_id ||
              page.owner.service !== owner?.service ||
              !sameFilter(page.filter, definition!.filter) ||
              page.after !== definition!.start_after
            )
              throw Error(t("订阅预览响应不符"));
            setPreview({ definition: definition!, page });
            setUncertain(false);
          })
        }
      >
        {t("预览订阅条件与起点")}
      </button>
      {preview && (
        <section aria-label={t("订阅条件预览")}>
          <p>
            {preview.definition.name} · {preview.definition.filter.contracts.join(", ")} ·{" "}
            {preview.definition.filter.use_scope}
          </p>
          <p>
            {t("源已确认位置")}：{preview.page.head} · {t("本页实际覆盖")}：{preview.page.after} →{" "}
            {preview.page.scanned_through}
          </p>
          <p>
            {t("本页匹配数")}：{preview.page.items.length} ·{" "}
            {t(preview.page.more ? "仍有范围尚未扫描" : "所选范围本页已覆盖")}
          </p>
          <p>{t("空匹配只说明已覆盖的明确条件，不代表市场没有事件。")}</p>
        </section>
      )}
      <button
        disabled={locked || (!draft.creating && !preview) || !!view}
        onClick={() =>
          void run(async token => {
            const definition = draft.creating ?? preview!.definition;
            setDraft(prev => ({ ...prev, creating: definition }));
            await context.trade("intelligence.subscription.create", { definition });
            if (token === epoch.current.value) await refresh(token, false, undefined, definition);
          })
        }
      >
        {t(draft.creating ? "重试原订阅启用" : "确认启用应用内订阅")}
      </button>
      <button
        disabled={locked || !owner || !draft.id}
        onClick={() => void run(token => refresh(token, true))}
      >
        {t("核实订阅及断连范围")}
      </button>
      {uncertain && (
        <p role="status">{t("当前结果待核实；保留原ID和批次，不据超时推断未执行或已补齐。")}</p>
      )}
      {view && (
        <section aria-label={t("订阅持久状态")}>
          <h3>{view.definition.name}</h3>
          <code>
            {view.reference.node}/{view.reference.service}/{view.reference.id}/
            {view.reference.version}
          </code>
          <p>
            {t(view.active ? "订阅已启用" : "订阅已暂停")} · {t("仅应用内提醒消费者")}
          </p>
          <p>
            {t("最后确认位置")}：{view.confirmed_through} · {t("源已确认位置")}：{view.head}
          </p>
          <p>
            {t("明确待补区间")}：
            {gap
              ? `${view.confirmed_through} → ${view.gap_through}`
              : t("无已登记缺口，未确认的新位置仍需交付")}
          </p>
          <p>
            {view.definition.filter.contracts.join(", ")} · {view.definition.filter.use_scope} ·{" "}
            {view.definition.filter.kinds.map(k => t(kindNames[k])).join(", ")}
          </p>
          <button disabled={locked || uncertain} onClick={() => void control(!view.active)}>
            {t(view.active ? "暂停订阅（保留记录）" : "恢复订阅并显示待补范围")}
          </button>
          <label>
            {t("交付终点（明确位置）")}
            <input
              value={draft.through}
              disabled={fixed || !!view.pending_delivery}
              inputMode="numeric"
              onChange={e => edit("through", e.target.value)}
            />
          </label>
          <button
            disabled={fixed || !!view.pending_delivery}
            onClick={() => edit("through", gap ? view.gap_through : view.head)}
          >
            {t("使用已观察到的待处理终点")}
          </button>
          <label>
            {t("交付操作 ID")}
            <input
              value={draft.offer?.operation_id ?? draft.operation}
              disabled={fixed || !!view.pending_delivery}
              onChange={e => edit("operation", e.target.value)}
            />
          </label>
          <button
            disabled={fixed || !!view.pending_delivery}
            onClick={() => edit("operation", crypto.randomUUID())}
          >
            {t("新建交付 ID")}
          </button>
          <label>
            <input
              type="checkbox"
              checked={draft.offer?.historical ?? draft.historical}
              disabled={fixed || !!view.pending_delivery}
              onChange={e => edit("historical", e.target.checked)}
            />
            {t("明确按历史补取交付")}
          </label>
          <button disabled={locked || (!draft.offer && !validOffer)} onClick={() => void offer()}>
            {t(draft.offer ? "重试原交付请求" : "按明确范围提供交付")}
          </button>
          {batch && (
            <section aria-label={t("原交付批次")}>
              <code>
                {batch.operation_id}/{batch.reference.version}
              </code>
              <p>{t(batch.historical ? "历史补取批次" : "原实时交付批次")}</p>
              <p>
                {t("原提供时间")}：{batch.offered_at_ms}
              </p>
              <p>
                {t("本批实际覆盖")}：{batch.after} → {batch.page.scanned_through} · {t("请求终点")}
                ：{batch.through}
              </p>
              <p>
                {t("本页匹配数")}：{batch.page.items.length} ·{" "}
                {t(batch.page.more ? "仍有范围尚未扫描" : "所选范围本页已覆盖")}
              </p>
              <p>{t("重复核实原批次不会新增提醒，原实时提供时间不会改为本次读取时间。")}</p>
              <button
                disabled={locked}
                onClick={() =>
                  void run(token =>
                    refresh(token, false, {
                      subscription: batch.subscription,
                      operation_id: batch.operation_id,
                      after: batch.after,
                      through: batch.through,
                      historical: batch.historical,
                    }),
                  )
                }
              >
                {t("只读核实原批次")}
              </button>
              <label>
                <input
                  type="checkbox"
                  checked={checked}
                  disabled={locked || batch.acknowledged}
                  onChange={e => setChecked(e.target.checked)}
                />
                {t("已核对本批覆盖范围与历史补取标签")}
              </label>
              <button
                disabled={locked || batch.acknowledged || !checked}
                onClick={() => void acknowledge()}
              >
                {t("确认原交付范围")}
              </button>
              {batch.acknowledged && (
                <p role="status">{t("该原批次已确认；这不是Research接纳或交易执行。")}</p>
              )}
            </section>
          )}
          <button
            disabled={locked}
            onClick={() => void run(token => loadInbox(token, view.reference, "0"))}
          >
            {t("读取已提供提醒记录")}
          </button>
          {inbox && (
            <section aria-label={t("应用内提醒记录")}>
              <p>{t("提供记录与最后确认位置分开；未确认记录不等于丢失，也不表示研究任务成功。")}</p>
              <ul>
                {inbox.items.map(item => (
                  <li
                    key={
                      item.change.source.kind + item.change.source.id + item.change.source.version
                    }
                  >
                    <p>
                      {t(kindNames[item.change.kind])} · {item.change.contract} ·{" "}
                      <code>{item.change.position}</code>
                    </p>
                    <code>
                      {item.change.event.id}/{item.change.source.id}/{item.change.source.version}
                    </code>
                    <p>
                      {t(item.historical ? "历史补取记录" : "原实时提供记录")} · {t("情报可用时间")}
                      ：{item.change.available_at_ms} · {t("首次提供时间")}：
                      {item.first_offered_at_ms}
                    </p>
                    <button
                      disabled={locked}
                      onClick={() =>
                        void run(async token => {
                          const reply = await context.query("intelligence.source.inspect", {
                            source: item.change.source,
                          });
                          if (token !== epoch.current.value) return;
                          if (
                            reply.intelligence_record?.published &&
                            same(reply.intelligence_record.published.reference, item.change.source)
                          )
                            setEvidence({
                              source: item.change.source,
                              record: reply.intelligence_record,
                            });
                          else if (
                            reply.notice_change?.published &&
                            same(reply.notice_change.published.reference, item.change.source)
                          )
                            setEvidence({
                              source: item.change.source,
                              change: reply.notice_change,
                            });
                          else throw Error(t("提醒原证据响应不符"));
                        })
                      }
                    >
                      {t("查看该变化的固定证据")}
                    </button>
                  </li>
                ))}
              </ul>
              {inbox.next_after !== "0" && (
                <button
                  disabled={locked}
                  onClick={() =>
                    void run(token => loadInbox(token, view.reference, inbox.next_after))
                  }
                >
                  {t("下一页提醒记录")}
                </button>
              )}
            </section>
          )}
        </section>
      )}
      {evidence && (
        <section aria-label={t("变化固定证据")}>
          <code>
            {evidence.source.id}/{evidence.source.version}
          </code>
          {evidence.record && (
            <>
              <p>
                {evidence.record.contract} · {evidence.record.use_scope} ·{" "}
                {evidence.record.use_mode}
              </p>
              <code>
                {evidence.record.material.reference.node}/
                {evidence.record.material.reference.service}/{evidence.record.material.reference.id}
                /{evidence.record.material.reference.version}
              </code>
              <ul>
                {Object.entries(evidence.record.evidence).map(([field, span]) => (
                  <li key={field}>
                    {field}：{span.text} [{span.begin},{span.end})
                  </li>
                ))}
              </ul>
            </>
          )}
          {evidence.change && (
            <>
              <p>
                {evidence.change.operator_name} · {evidence.change.reason}
              </p>
              <blockquote>{evidence.change.evidence.text}</blockquote>
              <code>
                {evidence.change.material.node}/{evidence.change.material.service}/
                {evidence.change.material.id}/{evidence.change.material.version}
              </code>
            </>
          )}
          <p>{t("这里读取原发布或变化的固定证据，不下载材料、不重新核验、不修改原情报。")}</p>
        </section>
      )}
      <ErrorNotice error={error} />
    </section>
  );
}
