import { useEffect, useRef, useState } from "react";
import type {
  ResearchModelMapping,
  ResearchModelMappingProjection,
  ResearchModelMappingSpecification,
  ResourceObject,
  TaskResult,
} from "../../src/bridge/client";
import {
  asDisplayError,
  ErrorNotice,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type TerminalContext,
} from "../contract";
import { t, useRun } from "./shared";
type Report = Extract<TaskResult, { kind: "model_training" | "model_time_validation" }>;
const empty = () => ({ name: "", purpose: "", short: "", long: "", middle: false, until: 0 });
function decimal(value: string): bigint | null {
  if (!/^-?\d+(?:\.\d{1,8})?$/.test(value)) return null;
  const sign = value.startsWith("-") ? -1n : 1n;
  const [whole, fraction = ""] = value.replace(/^-/, "").split(".");
  const raw = sign * (BigInt(whole) * 100000000n + BigInt(fraction.padEnd(8, "0")));
  return raw >= -9223372036854775808n && raw <= 9223372036854775807n ? raw : null;
}
const intentKey = (spec: ResearchModelMappingSpecification) =>
  JSON.stringify([
    spec.name,
    spec.purpose,
    [
      spec.model.node,
      spec.model.service,
      spec.model.domain,
      spec.model.kind,
      spec.model.id,
      spec.model.version,
    ],
    decimal(spec.short_threshold)?.toString(),
    decimal(spec.long_threshold)?.toString(),
    spec.middle,
    spec.valid_until_ms,
  ]);
export function ResearchModelMappings({
  snapshot,
  query,
  trade,
  busy,
  model,
  kind,
}: Pick<TerminalContext, "snapshot" | "query" | "trade" | "busy"> & {
  model: Report | null;
  kind: Report["kind"];
}) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([
    service?.node_id,
    service?.owner_node_id,
    service?.service,
    kind,
  ]);
  const title = kind === "model_time_validation" ? "逐折固定预测映射" : "固定预测映射";
  const current = useRef(destination);
  current.current = destination;
  const [draft, setDraft] = useWorkspaceDraft(`model-mapping-editor:${destination}`, empty);
  const [selected, setSelected] = useWorkspaceDraft<ResearchModelMapping | null>(
    `model-mapping-selected:${destination}`,
    null,
  );
  const [rows, setRows] = useState<ResearchModelMapping[]>([]);
  const [after, setAfter] = useState("");
  const [projection, setProjection] = useState<ResearchModelMappingProjection | null>(null);
  const [loading, setLoading] = useState(false);
  const { error, setError, run } = useRun(trade);
  const source: ResourceObject | null = model
    ? {
        ...model.experiment.plan.object,
        kind: `${kind}_result`,
        id: model.id,
        version: model.task.result_digest,
      }
    : null;
  const specification: ResearchModelMappingSpecification | null = source
    ? {
        name: draft.name,
        purpose: draft.purpose,
        model: source,
        short_threshold: draft.short,
        long_threshold: draft.long,
        middle: "no_new_direction",
        valid_until_ms: draft.until,
      }
    : null;
  const [requestId, setRequestId] = useWorkspaceRequestId(
    `model-mapping-save:${destination}`,
    JSON.stringify([draft, source]),
  );
  const dirty =
    Boolean(draft.name || draft.purpose || draft.short || draft.long || draft.until) &&
    (!draft.middle ||
      !selected ||
      !specification ||
      intentKey(selected.specification) !== intentKey(specification));
  useWorkspaceUnsaved(`model-mapping-editor:${destination}`, dirty);
  const blocked = busy || loading || !service?.online;
  const short = decimal(draft.short),
    long = decimal(draft.long);
  const valid =
    specification &&
    model?.kind === kind &&
    model.task.state === "succeeded" &&
    source?.node === service?.owner_node_id &&
    source?.service === service?.service &&
    !model.task.retired &&
    draft.name.trim() &&
    draft.purpose.trim() &&
    short !== null &&
    long !== null &&
    short < long &&
    draft.middle &&
    draft.until > Date.now() &&
    draft.until <= model.experiment.plan.specification.valid_until_ms;
  useEffect(() => {
    setRows([]);
    setAfter("");
    setProjection(null);
  }, [destination]);
  async function perform(action: () => Promise<void>) {
    setLoading(true);
    setError("");
    try {
      await action();
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setLoading(false);
    }
  }
  function edit<K extends keyof ReturnType<typeof empty>>(
    key: K,
    value: ReturnType<typeof empty>[K],
  ) {
    setDraft(old => ({ ...old, [key]: value }));
    setProjection(null);
  }
  async function list(cursor = "") {
    const owner = current.current;
    const reply = (
      await query("research.model.list", { kind: "mapping", after: cursor, limit: 30 })
    ).research_model;
    if (owner !== current.current) return;
    if (!reply) throw new Error(t("模型响应缺失"));
    const selected = reply.mappings.filter(
      value => value.specification.model.kind === `${kind}_result`,
    );
    setRows(old => (cursor ? [...old, ...selected] : selected));
    setAfter(reply.next_after);
  }
  async function load(object: ResourceObject) {
    const owner = current.current;
    const value = (await query("research.model.read", { kind: "mapping", id: object.id }))
      .research_model?.mapping;
    if (owner !== current.current) return;
    if (!value || value.object.version !== object.version || value.object.id !== object.id)
      throw new Error(t("映射确认与原声明不一致"));
    setSelected(value);
    setProjection(null);
  }
  async function save() {
    if (!specification || !valid) return;
    const owner = current.current,
      submitted = specification;
    const id = requestId ?? `model-mapping-${crypto.randomUUID()}`;
    setRequestId(id);
    const mappingId = `${id}-fixed`;
    if (
      !(await run("research.model.mapping.save", {
        id,
        mapping_id: mappingId,
        specification: submitted,
      }))
    )
      return;
    const value = (await query("research.model.read", { kind: "mapping", id: mappingId }))
      .research_model?.mapping;
    if (owner !== current.current) return;
    if (
      !value ||
      value.object.id !== mappingId ||
      intentKey(value.specification) !== intentKey(submitted)
    )
      throw new Error(t("映射确认与原声明不一致"));
    setSelected(value);
    setRequestId(null);
    setProjection(null);
    await list();
  }
  async function preview(begin: number) {
    if (!selected) return;
    const owner = current.current;
    const value = (
      await query("research.model.mapping.preview", { mapping: selected.object, begin, limit: 50 })
    ).research_model?.projection;
    if (owner !== current.current) return;
    if (
      !value ||
      value.mapping.object.version !== selected.object.version ||
      value.mapping.object.id !== selected.object.id
    )
      throw new Error(t("映射确认与原声明不一致"));
    setProjection(value);
  }
  const labels = {
    long_direction: "做多方向意图",
    short_direction: "做空方向意图",
    no_new_direction: "无新方向意图",
    prediction_unavailable: "预测不可用",
    expired: "映射已到期，禁止新增意图",
  } as const;
  return (
    <section aria-label={t(title)}>
      <h3>{t(title)}</h3>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      <p>{t("映射仅解释原事后诊断预测；无新方向不等于平仓，方向不等于资金权重，也不授权交易。")}</p>
      <p>
        {t("当前原模型")}: {source ? `${source.id} / ${source.version}` : t("请先读取完整成功模型")}
      </p>
      <label>
        {t("映射名称")}
        <input value={draft.name} disabled={blocked} onChange={e => edit("name", e.target.value)} />
      </label>
      <label>
        {t("映射用途")}
        <textarea
          value={draft.purpose}
          disabled={blocked}
          onChange={e => edit("purpose", e.target.value)}
        />
      </label>
      <label>
        {t("做空阈值（收益比率）")}
        <input
          value={draft.short}
          disabled={blocked}
          onChange={e => edit("short", e.target.value)}
        />
      </label>
      <label>
        {t("做多阈值（收益比率）")}
        <input value={draft.long} disabled={blocked} onChange={e => edit("long", e.target.value)} />
      </label>
      <label>
        <input
          type="checkbox"
          checked={draft.middle}
          disabled={blocked}
          onChange={e => edit("middle", e.target.checked)}
        />
        {t("中间区间无新方向意图，不表示平仓")}
      </label>
      <label>
        {t("映射适用截止（UTC）")}
        <input
          type="datetime-local"
          value={draft.until ? new Date(draft.until).toISOString().slice(0, 16) : ""}
          disabled={blocked}
          onChange={e => edit("until", e.target.value ? Date.parse(`${e.target.value}:00Z`) : 0)}
        />
      </label>
      <p>{t("固定后不可修改；阈值或用途变化另存映射，不改变原模型或旧引用。")}</p>
      <button disabled={blocked || !valid || !dirty} onClick={() => void perform(save)}>
        {t(title)}
      </button>
      <button disabled={blocked} onClick={() => void perform(() => list())}>
        {t("刷新固定映射")}
      </button>
      {rows.map(value => (
        <p key={value.object.id}>
          <button disabled={blocked} onClick={() => void perform(() => load(value.object))}>
            {value.specification.name} / {value.object.id}
          </button>
        </p>
      ))}
      {after && (
        <button disabled={blocked} onClick={() => void perform(() => list(after))}>
          {t("更多固定映射")}
        </button>
      )}
      {selected && (
        <section aria-label={t("已固定预测契约")}>
          <h4>{t("已固定预测契约")}</h4>
          <p>
            {selected.object.id} / {selected.object.version}
          </p>
          <p>
            {t("当前原模型")}: {selected.specification.model.id} /{" "}
            {selected.specification.model.version}
          </p>
          <p>
            {t("收益比率阈值（含边界）")}: ≤ {selected.specification.short_threshold} / ≥{" "}
            {selected.specification.long_threshold}
          </p>
          <p>{t("中间区间无新方向意图，不表示平仓")}</p>
          <p>
            {t("模型正式发布时间（UTC）")}: {new Date(selected.model_published_at_ms).toISOString()}
          </p>
          <p>
            {t("映射适用截止（UTC）")}:{" "}
            {new Date(selected.specification.valid_until_ms).toISOString()}
          </p>
          <button disabled={blocked} onClick={() => void perform(() => preview(0))}>
            {t("读取原行诊断映射")}
          </button>
        </section>
      )}
      {projection && (
        <section aria-label={t("原行诊断映射")}>
          <p>{t("历史可得时间未知；此处不是样本外评价或历史可交易信号。")}</p>
          <table>
            <thead>
              <tr>
                <th>{t("原行索引")}</th>
                <th>{t("行角色")}</th>
                <th>{t("诊断预测")}</th>
                <th>{t("方向意图")}</th>
              </tr>
            </thead>
            <tbody>
              {projection.rows.map(row => (
                <tr key={row.index}>
                  <td>{row.index}</td>
                  <td>
                    {row.role === "development_validation"
                      ? `${t("开发验证范围")} / ${t("折序号")} ${row.fold_index + 1} / ${t(
                          (
                            {
                              included: "参与开发验证",
                              warmup: "预热",
                              feature_unavailable: "特征缺失",
                              label_unavailable: "标签缺失",
                              crossed_label: "标签跨越验证截止",
                              non_finite_prediction: "预测数值不可用",
                            } as const
                          )[row.exclusion],
                        )}`
                      : t(
                          (
                            {
                              training: "训练范围内",
                              other_development: "其它开发行",
                              holdout: "原留出行",
                            } as const
                          )[row.role],
                        )}
                  </td>
                  <td>{row.prediction === null ? t("不可用") : row.prediction}</td>
                  <td>{t(labels[row.intent])}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <button
            disabled={blocked || projection.begin === 0}
            onClick={() => void perform(() => preview(Math.max(0, projection.begin - 50)))}
          >
            {t("上一页")}
          </button>
          <button
            disabled={blocked || projection.next_begin === null}
            onClick={() => void perform(() => preview(projection.next_begin ?? 0))}
          >
            {t("下一页")}
          </button>
        </section>
      )}
    </section>
  );
}
