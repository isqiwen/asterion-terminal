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
  NoticeCandidate,
  NoticeObject,
  NoticeRecord,
  NoticeSpan,
  TableCoverage,
  TableProduct,
  TableListedContracts,
} from "../../src/bridge/client";
import { NoticeConsumers } from "./NoticeConsumers";
import { NoticeChanges } from "./NoticeChanges";
const t = (key: string) => translate("asterion.terminal.intelligence", key);
const evidenceFields = {
  subject: "合约主体",
  parameter: "参数名称",
  effective_day: "计划生效日",
  percentage: "百分比",
  modality: "计划语气",
};
const humanFields = {
  subject: "主体映射无误",
  time: "日期和时间语义无误",
  quantity_unit: "数量与单位无误",
  modality_context: "否定、条件和计划语气已核对",
  source_use: "材料使用范围已核对",
  conflicts: "冲突信息已核对",
};
const states = {
  preparing: "准备中",
  draft: "待人工核验",
  verified: "已核验待发布",
  publishing: "待保存确认",
  published: "已发布",
  rejected: "已拒绝",
  abandoned: "已放弃",
};
export function NoticeReview({ context }: { context: TerminalContext }) {
  const data = context.snapshot?.data,
    owner = context.snapshot?.intelligence_service;
  const [materialId, setMaterialId] = useState(context.workspaceParams?.material ?? "");
  const [id, setId] = useState(context.workspaceParams?.id ?? "");
  const [material, setMaterial] = useState<MaterialRead | null>(null);
  const [coverage, setCoverage] = useState<TableCoverage | null>(null);
  const [source, setSource] = useState("");
  const [products, setProducts] = useState<TableProduct[]>([]);
  const [product, setProduct] = useState("");
  const [dictionary, setDictionary] = useState<TableListedContracts | null>(null);
  const [contract, setContract] = useState("");
  const [mode, setMode] = useState<NoticeCandidate["use_mode"]>("post_hoc_research");
  const [selection, setSelection] = useState<NoticeSpan | null>(null);
  const emptyEvidence = (): NoticeCandidate["evidence"] =>
    Object.fromEntries(
      Object.keys(evidenceFields).map(k => [k, { begin: 0, end: 0, text: "" }]),
    ) as NoticeCandidate["evidence"];
  const [evidence, setEvidence] = useState(emptyEvidence);
  const [record, setRecord] = useState<NoticeRecord | null>(null);
  const [followupEvent, setFollowupEvent] = useState<NoticeObject | null>(null);
  const [rows, setRows] = useState<NoticeRecord[]>([]);
  const [operator, setOperator] = useState(""),
    [reason, setReason] = useState("");
  const [publishReason, setPublishReason] = useState("");
  const [checks, setChecks] = useState<string[]>([]);
  const [operations, setOperations] = useState({ review: "", publish: "", abandon: "" });
  const [pending, setPending] = useState(false),
    [error, setError] = useState<DisplayError>("");
  const epoch = useRef(0);
  const ownerKey = owner?.connection_id,
    dataKey = data?.connection_id;
  useEffect(() => {
    ++epoch.current;
    setMaterial(null);
    setCoverage(null);
    setSource("");
    setProducts([]);
    setDictionary(null);
    setContract("");
    setSelection(null);
    setEvidence(emptyEvidence());
    setRecord(null);
    setRows([]);
    setFollowupEvent(null);
    setChecks([]);
    setPending(false);
    setError("");
  }, [ownerKey, dataKey]);
  const routeId = context.workspaceParams?.id,
    routeMaterial = context.workspaceParams?.material;
  useEffect(() => {
    ++epoch.current;
    setId(routeId ?? "");
    setMaterialId(routeMaterial ?? "");
    setRecord(null);
    setMaterial(null);
    setSelection(null);
    setChecks([]);
    setPending(false);
  }, [routeId, routeMaterial]);
  useEffect(
    () => () => {
      ++epoch.current;
    },
    [],
  );
  const accept = (r: NoticeRecord) => {
    if (r.reference.version !== record?.reference.version) {
      setMaterialId(r.material.reference.id);
      setChecks([]);
      setReason("");
      setOperator("");
      setPublishReason(r.publication_request?.reason ?? "");
      setOperations({
        review: crypto.randomUUID(),
        publish: r.publication_request?.id ?? crypto.randomUUID(),
        abandon: crypto.randomUUID(),
      });
    }
    setRecord(r);
    setFollowupEvent(r.related_event ?? null);
  };
  async function run(work: (current: number) => Promise<void>) {
    const current = ++epoch.current;
    setPending(true);
    setError("");
    try {
      await work(current);
    } catch (e) {
      if (current === epoch.current) setError(asDisplayError(e));
    } finally {
      if (current === epoch.current) setPending(false);
    }
  }
  async function inspect(current: number, candidateId = id) {
    const reply = await context.query("intelligence.get", { id: candidateId });
    if (!reply.intelligence_record) throw Error(t("候选响应缺失"));
    if (current === epoch.current) accept(reply.intelligence_record);
  }
  const sources = [
    ...new Set(
      (coverage?.rows ?? [])
        .filter(row => row.table === "futures.contracts")
        .map(row => row.source),
    ),
  ];
  const canReview = record?.state === "draft" && record.assessment.ready_for_human_review;
  const originalMatches =
    !!material?.text &&
    material.reading.state === "readable" &&
    !!record &&
    Object.entries(record.material.reference).every(
      ([key, value]) =>
        material.receipt.reference[key as keyof typeof material.receipt.reference] === value,
    );
  const ready =
    originalMatches &&
    canReview &&
    checks.length === Object.keys(humanFields).length &&
    !!operator.trim() &&
    !!reason.trim();
  return (
    <section aria-label={t("人工公告核验")}>
      <h3>{t("人工公告核验")}</h3>
      <p>
        {t("仅支持原文明示合约、明确日期、保证金百分比及计划语气的公告。人工发布不改变交易参数。")}
      </p>
      <p>
        {t("当前情报服务")}：<code>{owner ? owner.node_id + "/" + owner.service : "—"}</code>
      </p>
      <button
        disabled={pending || context.busy}
        onClick={() =>
          void run(async () => {
            await context.trade("node.intelligence.local.open", {});
          })
        }
      >
        {t("打开本机情报服务")}
      </button>
      <button
        disabled={pending || !owner}
        onClick={() =>
          void run(async current => {
            const reply = await context.query("intelligence.list", { after: "" });
            if (current === epoch.current) setRows(reply.intelligence_records ?? []);
          })
        }
      >
        {t("刷新候选")}
      </button>
      {rows.map(row => (
        <button
          key={row.reference.version}
          disabled={pending}
          onClick={() => {
            setId(row.reference.id);
            accept(row);
          }}
        >
          {row.reference.id} · {t(states[row.state])}
        </button>
      ))}
      <fieldset disabled={pending}>
        <legend>{t("固定来源与证据")}</legend>
        <p>
          {t("当前 Data")}：
          <code>
            {data?.node_id ?? "—"}/{data?.service ?? "—"}
          </code>
        </p>
        <label>
          {t("材料操作 ID")}
          <input
            value={materialId}
            onChange={e => {
              setMaterialId(e.target.value);
              setMaterial(null);
              setSelection(null);
              setEvidence(emptyEvidence());
            }}
          />
        </label>
        <button
          disabled={!data?.online || !materialId}
          onClick={() =>
            void run(async current => {
              const reply = await context.query("data.material.read", { id: materialId });
              if (!reply.material_read) throw Error(t("固定材料响应缺失"));
              if (current === epoch.current) {
                setMaterial(reply.material_read);
                setEvidence(emptyEvidence());
                setSelection(null);
              }
            })
          }
        >
          {t("读取固定材料")}
        </button>
        {material && (
          <div>
            <p>
              {t("内容版本")}：<code>{material.receipt.content.version}</code>
            </p>
            <p>
              {t("使用范围")}：{material.receipt.request.use_scope}
            </p>
            <p>
              {t("来源发布时间")}：{material.receipt.source_published_at_ms ?? t("未知")}
            </p>
            {material.reading.state === "readable" ? (
              <textarea
                aria-label={t("材料原文")}
                readOnly
                rows={8}
                value={material.text ?? ""}
                onSelect={e => {
                  const text = material.text ?? "",
                    el = e.currentTarget;
                  const begin = new TextEncoder().encode(text.slice(0, el.selectionStart)).length;
                  const end = new TextEncoder().encode(text.slice(0, el.selectionEnd)).length;
                  setSelection(
                    begin < end
                      ? { begin, end, text: text.slice(el.selectionStart, el.selectionEnd) }
                      : null,
                  );
                }}
              />
            ) : (
              <p>{t("材料尚不可读，不能核验发布。")}</p>
            )}
            {Object.entries(evidenceFields).map(([key, label]) => (
              <div key={key}>
                <button
                  disabled={!selection}
                  onClick={() => setEvidence(prev => ({ ...prev, [key]: selection! }))}
                >
                  {t("使用选中文本")} · {t(label)}
                </button>
                <span>{evidence[key as keyof typeof evidence].text || "—"}</span>
              </div>
            ))}
          </div>
        )}
        <button
          disabled={!data?.online}
          onClick={() =>
            void run(async current => {
              const reply = await context.query("data.tables.coverage", {});
              if (!reply.table_coverage) throw Error(t("固定合约资料响应缺失"));
              if (current === epoch.current) {
                setCoverage(reply.table_coverage);
                setSource("");
                setProducts([]);
                setDictionary(null);
                setContract("");
              }
            })
          }
        >
          {t("读取固定合约资料")}
        </button>
        <label>
          {t("合约资料来源")}
          <select
            value={source}
            onChange={e => {
              setSource(e.target.value);
              setProducts([]);
              setDictionary(null);
              setContract("");
            }}
          >
            <option value="">{t("请选择")}</option>
            {sources.map(s => (
              <option key={s}>{s}</option>
            ))}
          </select>
        </label>
        <button
          disabled={!source || !coverage}
          onClick={() =>
            void run(async current => {
              const reply = await context.query("data.tables.products", {
                snapshot: coverage!.snapshot,
                contracts_source: source,
              });
              if (current === epoch.current) {
                setProducts(reply.table_products ?? []);
                setProduct("");
                setDictionary(null);
                setContract("");
              }
            })
          }
        >
          {t("读取品种")}
        </button>
        <label>
          {t("品种")}
          <select
            value={product}
            onChange={e => {
              setProduct(e.target.value);
              setDictionary(null);
              setContract("");
            }}
          >
            <option value="">{t("请选择")}</option>
            {products.map(p => (
              <option key={p.exchange + "/" + p.product} value={p.exchange + "/" + p.product}>
                {p.exchange} / {p.product}
              </option>
            ))}
          </select>
        </label>
        <button
          disabled={!product || !coverage}
          onClick={() =>
            void run(async current => {
              const [exchange, productId] = product.split("/");
              const reply = await context.query("data.tables.listed_contracts", {
                snapshot: coverage!.snapshot,
                contracts_source: source,
                exchange,
                product: productId,
              });
              if (current === epoch.current) {
                setDictionary(reply.table_listed_contracts ?? null);
                setContract("");
              }
            })
          }
        >
          {t("读取合约")}
        </button>
        <label>
          {t("合约")}
          <select value={contract} onChange={e => setContract(e.target.value)}>
            <option value="">{t("请选择")}</option>
            {dictionary?.items.map(c => (
              <option key={c.contract_id} value={c.contract_id}>
                {c.name} · {c.contract_id}
              </option>
            ))}
          </select>
        </label>
        <label>
          {t("候选 ID")}
          <input value={id} onChange={e => setId(e.target.value)} />
        </label>
        <label>
          {t("研究用途")}
          <select value={mode} onChange={e => setMode(e.target.value as typeof mode)}>
            <option value="post_hoc_research">{t("事后研究")}</option>
            <option value="strict_history">{t("严格历史")}</option>
          </select>
        </label>
        <button
          disabled={
            !owner ||
            !material?.text ||
            !dictionary ||
            !contract ||
            !id ||
            Object.values(evidence).some(s => !s.text)
          }
          onClick={() =>
            void run(async current => {
              await context.trade("intelligence.submit", {
                candidate: {
                  id,
                  material: material!.receipt.reference,
                  dictionary: dictionary!.query,
                  contract,
                  evidence,
                  rule_id: "contract_margin_announcement",
                  use_scope: material!.receipt.request.use_scope,
                  use_mode: mode,
                  event: followupEvent,
                },
              });
              await inspect(current);
            })
          }
        >
          {t("提交固定候选")}
        </button>
        <button disabled={!owner || !id} onClick={() => void run(current => inspect(current))}>
          {t("读取候选状态")}
        </button>
      </fieldset>
      {followupEvent && (
        <p>
          {t("后续陈述明确关联事件")}：<code>{followupEvent.id}</code>
          <button disabled={pending} onClick={() => setFollowupEvent(null)}>
            {t("取消事件关联")}
          </button>
        </p>
      )}
      {record && (
        <div>
          {!originalMatches && <p>{t("请读取该候选的固定原文后逐项核验。")}</p>}
          <p>
            <code>
              {record.material.reference.node}/{record.material.reference.service}/
              {record.material.reference.id}
            </code>
          </p>
          <h4>
            {record.reference.id} · {t(states[record.state])}
          </h4>
          <p>
            {record.contract} · {record.assessment.effective_day} ·{" "}
            {record.assessment.margin_ratio ?? "—"}
          </p>
          <p>
            {t("候选版本")}：<code>{record.reference.version}</code>
          </p>
          <ul>
            {record.assessment.checks.map(c => (
              <li key={c.field}>
                {t(c.state)} · {t(c.reason)}
              </li>
            ))}
          </ul>
          {record.assessment.limitations.map(l => (
            <p key={l}>{t(l)}</p>
          ))}
          <fieldset disabled={pending || record.state !== "draft"}>
            <legend>{t("人工逐项复核")}</legend>
            {Object.entries(humanFields).map(([key, label]) => (
              <label key={key}>
                <input
                  type="checkbox"
                  checked={checks.includes(key)}
                  onChange={e =>
                    setChecks(prev =>
                      e.target.checked ? [...prev, key] : prev.filter(k => k !== key),
                    )
                  }
                />
                {t(label)}
              </label>
            ))}
            <label>
              {t("核验人")}
              <input value={operator} onChange={e => setOperator(e.target.value)} />
            </label>
            <label>
              {t("核验理由")}
              <textarea value={reason} onChange={e => setReason(e.target.value)} />
            </label>
            <button
              disabled={!ready}
              onClick={() =>
                void run(async current => {
                  await context.trade("intelligence.review", {
                    id: operations.review,
                    candidate: record.reference,
                    operator_name: operator,
                    reason,
                    approve: true,
                    checks,
                  });
                  await inspect(current, record.reference.id);
                })
              }
            >
              {t("确认核验通过")}
            </button>
            <button
              disabled={!operator.trim() || !reason.trim()}
              onClick={() =>
                void run(async current => {
                  await context.trade("intelligence.review", {
                    id: operations.review,
                    candidate: record.reference,
                    operator_name: operator,
                    reason,
                    approve: false,
                    checks,
                  });
                  await inspect(current, record.reference.id);
                })
              }
            >
              {t("拒绝候选")}
            </button>
          </fieldset>
          {record.verification && (
            <p>
              {t("人工决定")}：{record.verification.operator_name} · {record.verification.reason} ·{" "}
              {record.verification.verified_at_ms}
            </p>
          )}
          {(record.state === "verified" || record.state === "publishing") && (
            <div>
              <label>
                {t("发布理由")}
                <textarea
                  disabled={pending || record.state === "publishing"}
                  value={record.publication_request?.reason ?? publishReason}
                  onChange={e => setPublishReason(e.target.value)}
                />
              </label>
              <button
                disabled={pending || !(record.publication_request?.reason ?? publishReason).trim()}
                onClick={() =>
                  void run(async current => {
                    const source = record.material.reference;
                    await context.trade("intelligence.publish", {
                      id: record.publication_request?.id ?? operations.publish,
                      candidate: record.reference,
                      data_owner: {
                        node: source.node,
                        service: source.service,
                        domain: source.domain,
                      },
                      reason: record.publication_request?.reason ?? publishReason,
                    });
                    await inspect(current, record.reference.id);
                  })
                }
              >
                {t(record.state === "publishing" ? "重试保存确认" : "请求保存并发布")}
              </button>
            </div>
          )}
          {record.published && (
            <div>
              <button
                disabled={pending}
                onClick={() =>
                  context.navigate("workspace.notice-review", {
                    params: {
                      source: JSON.stringify(record.published!.reference),
                      scope: record.use_scope,
                    },
                  })
                }
              >
                {t("交给 Research 审阅")}
              </button>
              <p>
                {t("Data 保存时间")}：{record.published.stored_at_ms}
              </p>
              <p>
                {t("情报可用时间")}：{record.published.available_at_ms}
              </p>
              <p>
                {t("发布身份")}：<code>{record.published.reference.version}</code>
              </p>
              {record.event && (
                <button
                  disabled={pending}
                  onClick={() => {
                    setFollowupEvent(record.event!.reference);
                    setId("");
                    setRecord(null);
                    setMaterial(null);
                    setMaterialId("");
                    setSelection(null);
                    setEvidence(emptyEvidence());
                    setChecks([]);
                  }}
                >
                  {t("同事件新建后续陈述")}
                </button>
              )}
              <NoticeConsumers context={context} source={record.published.reference} />
              <NoticeChanges context={context} record={record} />
            </div>
          )}
        </div>
      )}
      <ErrorNotice error={error} />
    </section>
  );
}
