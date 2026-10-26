import { useCallback, useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type LogicStrategyPackage,
  type LogicStrategyDelivery,
  type TaskResult,
  type PositionSides,
} from "../../src/bridge/client";
import {
  asDisplayError,
  strategySize,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  type TerminalContext,
} from "../contract";
import { ResearchPage, t, useRun } from "./shared";
type Trial = Extract<TaskResult, { kind: "logic_trial" }>;
const fresh = () => ({
  id: `logic-package-${crypto.randomUUID()}`,
  name: "",
  purpose: "",
  limitations: "",
  source: "",
  quantity: "",
  sides: "both" as PositionSides,
  dataset: "",
});
const draftIds = () => ({
  operation: `logic-delivery-operation-${crypto.randomUUID()}`,
  id: `logic-delivery-${crypto.randomUUID()}`,
  target: `logic-backtest-${crypto.randomUUID()}`,
});
export function LogicStrategyPackages({
  snapshot,
  query,
  trade,
  busy,
  navigate,
  workspaceParams,
}: TerminalContext) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([
    service?.owner_node_id,
    service?.service,
    service?.connection_id,
  ]);
  const [form, setForm] = useWorkspaceDraft(`logic-package-editor:${destination}`, fresh);
  const [selected, setSelected] = useWorkspaceDraft<LogicStrategyPackage | null>(
    `logic-package-selected:${destination}`,
    null,
  );
  const [source, setSource] = useState<Trial | null>(null),
    [items, setItems] = useState<LogicStrategyPackage[]>([]),
    [after, setAfter] = useState("");
  const scope = `${destination}:${selected?.object.id ?? ""}`;
  const [ids, setIds] = useWorkspaceDraft(`logic-delivery-ids:${scope}`, draftIds);
  const [draft, setDraft] = useWorkspaceDraft<LogicStrategyDelivery | null>(
    `logic-delivery:${scope}`,
    null,
  );
  const [drafts, setDrafts] = useState<LogicStrategyDelivery[]>([]),
    [nextDraft, setNextDraft] = useState("");
  const [accepted, setAccepted] = useState<boolean | null>(null),
    [loading, setLoading] = useState(false),
    [reason, setReason] = useState("");
  const [freezeId, setFreezeId] = useWorkspaceRequestId(
    `logic-package-freeze:${destination}`,
    JSON.stringify(form),
  );
  const [retireId, setRetireId] = useWorkspaceRequestId(`logic-package-retire:${scope}`, reason);
  const { run, error, setError } = useRun(trade);
  const ask = useRef(query);
  ask.current = query;
  const owner = useRef(destination);
  owner.current = destination;
  const blocked = busy || loading || !service?.online;
  const list = useCallback(async (cursor = "") => {
    const original = owner.current;
    const value = (await ask.current("research.strategy.list", { after: cursor, limit: 30 }))
      .research_strategy;
    if (original !== owner.current) return;
    if (!value) throw new Error(t("策略响应缺失"));
    const rows = value.packages.filter(
      (item): item is LogicStrategyPackage => item.evidence_relation === "logic_trial",
    );
    setItems(previous => (cursor ? [...previous, ...rows] : rows));
    setAfter(value.next_after);
  }, []);
  const listDrafts = useCallback(
    async (cursor = "") => {
      if (!selected) return;
      const original = owner.current;
      const value = (
        await ask.current("research.strategy.delivery.list", {
          package_id: selected.object.id,
          after: cursor,
          limit: 30,
        })
      ).research_strategy;
      if (original !== owner.current) return;
      if (!value) throw new Error(t("策略响应缺失"));
      const rows = value.deliveries.filter(
        (item): item is LogicStrategyDelivery => item.reference_result === null,
      );
      setDrafts(previous => (cursor ? [...previous, ...rows] : rows));
      setNextDraft(value.next_after);
    },
    [selected],
  );
  useEffect(() => {
    setSource(null);
    setItems([]);
    setAccepted(null);
    void list().catch(e => setError(asDisplayError(e)));
  }, [destination, list, setError]);
  useEffect(() => {
    setDrafts([]);
    setAccepted(null);
    void listDrafts().catch(e => setError(asDisplayError(e)));
  }, [listDrafts, setError]);
  const incoming = workspaceParams?.logic_trial_id;
  useEffect(() => {
    if (incoming) {
      setSelected(null);
      setForm(previous => ({ ...previous, source: incoming }));
      setSource(null);
    }
  }, [incoming, destination, setSelected, setForm]);
  async function perform(work: () => Promise<void>) {
    setLoading(true);
    setError("");
    try {
      await work();
    } catch (e) {
      setError(asDisplayError(e));
    } finally {
      setLoading(false);
    }
  }
  async function inspect(id: string) {
    const value = (await query("research.strategy.read", { id })).research_strategy?.package;
    if (!value || value.evidence_relation !== "logic_trial") throw new Error(t("策略响应缺失"));
    setSelected(value);
  }
  async function inspectDraft(id: string) {
    const value = (await query("research.strategy.delivery.read", { id })).research_strategy;
    if (!value?.delivery || value.delivery.reference_result !== null)
      throw new Error(t("策略响应缺失"));
    setDraft(value.delivery);
    setAccepted(value.target_accepted ?? false);
  }
  async function inspectSource() {
    if (!service) return;
    const id = selected?.logic.result.id ?? form.source;
    const value = (await query("task.result.read", { owner: taskOwner(service, "research"), id }))
      .task_result;
    if (
      !value ||
      value.kind !== "logic_trial" ||
      value.id !== id ||
      !value.result.all_match ||
      value.result.decisions === 0 ||
      (!selected && value.task.retired) ||
      (selected &&
        (value.task.result_digest !== selected.logic.result.version ||
          value.input_sha256 !== selected.source_input_sha256))
    )
      throw new Error(t("组包需要一致且含实际方向的已匹配试算"));
    setSource(value);
  }
  const datasets = snapshot?.datasets ?? [];
  const dataset = datasets.find(item => item.revision === form.dataset);
  const supported =
    source && source.id === form.source && source.result.all_match && source.result.decisions > 0;
  return (
    <ResearchPage taskService={service} error={error}>
      <p>{t("包引用原规则和已匹配虚构试算，不是市场收益证据。保存包和交付草稿均不运行回测。")}</p>
      <button
        disabled={blocked}
        onClick={() => {
          setSelected(null);
          setForm(fresh());
          setSource(null);
          setDraft(null);
        }}
      >
        {t("新建逻辑策略包")}
      </button>
      <button disabled={blocked} onClick={() => void perform(() => list())}>
        {t("刷新候选列表")}
      </button>
      <ul>
        {items.map(item => (
          <li key={item.object.id}>
            <button disabled={blocked} onClick={() => void perform(() => inspect(item.object.id))}>
              {item.name} · {item.object.id}
            </button>
            {item.retirement_reason && (
              <span>
                {t("已弃用")} · {item.retirement_reason}
              </span>
            )}
          </li>
        ))}
      </ul>
      {after && (
        <button disabled={blocked} onClick={() => void perform(() => list(after))}>
          {t("更多")}
        </button>
      )}
      {!selected ? (
        <fieldset disabled={blocked}>
          <legend>{t("从已核对逻辑组装")}</legend>
          <label>
            {t("来源试算任务")}
            <input
              value={form.source}
              onChange={e => {
                setForm({ ...form, source: e.target.value });
                setSource(null);
              }}
            />
          </label>
          <button disabled={!form.source.trim()} onClick={() => void perform(inspectSource)}>
            {t("读取已核对试算")}
          </button>
          {source && (
            <p>
              {source.experiment.definition.name} · {source.experiment.definition.fast}/
              {source.experiment.definition.slow} · {t("实际方向行数")} {source.result.decisions} ·{" "}
              <code>{source.task.program_artifact}</code>
            </p>
          )}
          <label>
            {t("候选名称")}
            <input value={form.name} onChange={e => setForm({ ...form, name: e.target.value })} />
          </label>
          <label>
            {t("候选用途")}
            <input
              value={form.purpose}
              onChange={e => setForm({ ...form, purpose: e.target.value })}
            />
          </label>
          <label>
            {t("候选限制")}
            <input
              value={form.limitations}
              onChange={e => setForm({ ...form, limitations: e.target.value })}
            />
          </label>
          <label>
            {t("组包合约与周期")}
            <select
              value={form.dataset}
              onChange={e => setForm({ ...form, dataset: e.target.value })}
            >
              <option value="">{t("请选择")}</option>
              {datasets
                .filter(item => item.interval_minutes === 0 || item.interval_minutes === 1)
                .map(item => (
                  <option key={item.revision} value={item.revision}>
                    {item.venue}/{item.symbol} ·{" "}
                    {item.interval_minutes === 0 ? t("日线") : t("1分钟")}
                  </option>
                ))}
            </select>
          </label>
          <p>
            {t(
              "从已选数据读取合约单位和周期；市场数据范围、资金、费用和风险仍须在目标回测草稿确认。",
            )}
          </p>
          <button onClick={() => navigate("workspace.history")}>{t("选择已有合约数据")}</button>
          <label>
            {t("固定目标数量")}
            <input
              value={form.quantity}
              onChange={e => setForm({ ...form, quantity: e.target.value })}
            />
          </label>
          <label>
            {t("允许目标方向")}
            <select
              value={form.sides}
              onChange={e =>
                setForm({
                  ...form,
                  sides:
                    e.target.value === "long"
                      ? "long"
                      : e.target.value === "short"
                        ? "short"
                        : "both",
                })
              }
            >
              <option value="both">{t("双向")}</option>
              <option value="long">{t("只做多")}</option>
              <option value="short">{t("只做空")}</option>
            </select>
          </label>
          <p>
            {t(
              "目标符号乘固定数量，再受允许方向约束；零仍表示平仓目标，实际成交由回测引擎和风险决定。",
            )}
          </p>
          <button
            disabled={
              !supported ||
              !dataset ||
              !form.name.trim() ||
              !form.purpose.trim() ||
              !form.limitations.trim() ||
              !form.quantity.trim()
            }
            onClick={() =>
              void perform(async () => {
                if (!source || !dataset || !service?.owner_node_id) return;
                const id = freezeId ?? `logic-package-freeze-${crypto.randomUUID()}`;
                setFreezeId(id);
                if (
                  !(await run("research.strategy.logic.freeze", {
                    id,
                    registration: {
                      id: form.id,
                      name: form.name,
                      purpose: form.purpose,
                      limitations: form.limitations,
                      source: {
                        node: service.owner_node_id,
                        service: service.service,
                        domain: "research",
                        kind: "logic_trial_result",
                        id: source.id,
                        version: source.task.result_digest,
                      },
                      contract: dataset.contract,
                      interval_minutes: dataset.interval_minutes,
                      strategy: {
                        quantity: form.quantity,
                        sides: form.sides,
                        rule: {
                          kind: "moving_average",
                          fast: source.experiment.definition.fast,
                          slow: source.experiment.definition.slow,
                        },
                      },
                    },
                  }))
                )
                  return;
                await inspect(form.id);
                await list();
              })
            }
          >
            {t("保存已核对逻辑包，不运行")}
          </button>
        </fieldset>
      ) : (
        <>
          <section aria-label={t("固定逻辑包定义")}>
            <h3>{selected.name}</h3>
            <p>{selected.purpose}</p>
            <p>{selected.limitations}</p>
            <p>
              {selected.logic.definition.name} · {selected.logic.definition.fast}/
              {selected.logic.definition.slow} · {selected.definition.contract.venue}/
              {selected.definition.contract.symbol} · {t("固定目标数量")}{" "}
              {strategySize(selected.definition.strategy)}
            </p>
            <p>
              {t("原规则版本")} <code>{selected.logic.definition.object.version}</code>
            </p>
            <p>
              {t("原试算任务")} <code>{selected.logic.result.id}</code> ·{" "}
              <code>{selected.logic.result.version}</code>
            </p>
            <p>
              {t("原执行程序")} <code>{selected.definition.program_artifact}</code>
            </p>
            <p>
              {t("原风险实现")} <code>{selected.definition.risk_artifact}</code>
            </p>
            <p>
              {selected.retirement_reason
                ? `${t("已弃用")} · ${selected.retirement_reason}`
                : t("可创建交付草稿")}
            </p>
            <button disabled={blocked} onClick={() => void perform(inspectSource)}>
              {t("读取原试算证据")}
            </button>
            <label>
              {t("停用原因")}
              <input value={reason} onChange={e => setReason(e.target.value)} />
            </label>
            <button
              disabled={blocked || !reason.trim() || Boolean(selected.retirement_reason)}
              onClick={() =>
                void perform(async () => {
                  const id = retireId ?? `logic-package-retire-${crypto.randomUUID()}`;
                  setRetireId(id);
                  if (
                    await run("research.strategy.deprecate", {
                      id,
                      package: selected.object,
                      reason,
                    })
                  ) {
                    await inspect(selected.object.id);
                    await list();
                  }
                })
              }
            >
              {t("弃用候选，保留既有任务")}
            </button>
          </section>
          <ul>
            {drafts.map(value => (
              <li key={value.object.id}>
                <button
                  disabled={blocked}
                  onClick={() => void perform(() => inspectDraft(value.object.id))}
                >
                  {value.target_task_id}
                </button>
              </li>
            ))}
          </ul>
          {nextDraft && (
            <button disabled={blocked} onClick={() => void perform(() => listDrafts(nextDraft))}>
              {t("更多交付草稿")}
            </button>
          )}
          <button
            disabled={blocked || Boolean(selected.retirement_reason)}
            onClick={() => {
              setDraft(null);
              setIds(draftIds());
              setAccepted(null);
            }}
          >
            {t("新建交付草稿")}
          </button>
          <fieldset disabled={blocked || Boolean(selected.retirement_reason) || Boolean(draft)}>
            <legend>{t("固定目标交付草稿")}</legend>
            <label>
              {t("目标回测任务")}
              <input
                value={ids.target}
                onChange={e => setIds({ ...ids, target: e.target.value })}
              />
            </label>
            <button
              disabled={!ids.target.trim()}
              onClick={() =>
                void perform(async () => {
                  if (
                    await run("research.strategy.delivery.create", {
                      id: ids.operation,
                      delivery: {
                        id: ids.id,
                        package: selected.object,
                        target_task_id: ids.target,
                      },
                    })
                  ) {
                    await inspectDraft(ids.id);
                    await listDrafts();
                  }
                })
              }
            >
              {t("保存交付草稿，不运行")}
            </button>
          </fieldset>
          {draft && (
            <section aria-label={t("逻辑交付状态")}>
              <p>
                <code>{draft.object.id}</code> · <code>{draft.target_task_id}</code>
              </p>
              <p>
                {accepted === true
                  ? t("Research已接纳；不代表任务成功")
                  : accepted === false
                    ? t("草稿已保存，尚无正式接纳")
                    : t("尚未核对接纳状态")}
              </p>
              <button
                disabled={blocked}
                onClick={() => void perform(() => inspectDraft(draft.object.id))}
              >
                {t("查询原草稿与接纳状态")}
              </button>
              <button
                disabled={blocked || Boolean(selected.retirement_reason) || accepted === true}
                onClick={() =>
                  navigate("workspace.backtest", {
                    page: "backtest",
                    params: { strategy_delivery_id: draft.object.id },
                  })
                }
              >
                {t("配置交付回测草稿")}
              </button>
              <button
                disabled={blocked || accepted !== true}
                onClick={() =>
                  void perform(async () => {
                    if (
                      await run("task.result", {
                        owner: taskOwner(service, "research"),
                        id: draft.target_task_id,
                      })
                    )
                      navigate("workspace.backtest", {
                        page: "backtest",
                        params: { result_id: draft.target_task_id },
                      });
                  })
                }
              >
                {t("查看交付回测正式结果")}
              </button>
            </section>
          )}
        </>
      )}
    </ResearchPage>
  );
}
