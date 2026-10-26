import { Fragment, useCallback, useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type ReferenceStrategyPackage,
  type ReferenceStrategyDelivery,
  type TaskResult,
} from "../../src/bridge/client";
import {
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  strategyRule,
  strategyRows,
  type TerminalContext,
} from "../contract";
import { ResearchPage, t, useRun, states } from "./shared";
import { ExperimentDetails } from "./ExperimentDetails";
import { LogicStrategyPackages } from "./LogicStrategyPackages";
type Evidence = Extract<TaskResult, { kind: "backtest" }>;
const fresh = () => ({
  id: `strategy-${crypto.randomUUID()}`,
  name: "",
  purpose: "",
  limitations: "",
  source: "",
});
export function StrategyPackages(context: TerminalContext) {
  const [mode, setMode] = useWorkspaceDraft<"reference" | "logic">(
    `strategy-origin:${context.snapshot?.research_service?.connection_id}`,
    "reference",
  );
  const incoming = context.workspaceParams?.logic_trial_id;
  useEffect(() => {
    if (incoming) setMode("logic");
  }, [incoming, setMode]);
  return (
    <>
      <div className="research-inputs strategy-origin" role="group" aria-label={t("策略包来源")}>
        <button aria-pressed={mode === "reference"} onClick={() => setMode("reference")}>
          {t("回测参考候选")}
        </button>
        <button aria-pressed={mode === "logic"} onClick={() => setMode("logic")}>
          {t("已核对逻辑包")}
        </button>
      </div>
      {mode === "logic" ? (
        <LogicStrategyPackages {...context} />
      ) : (
        <ReferenceStrategyPackages {...context} />
      )}
    </>
  );
}
function ReferenceStrategyPackages({ snapshot, query, trade, busy, navigate }: TerminalContext) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([
    service?.owner_node_id,
    service?.service,
    service?.connection_id,
  ]);
  const [editor, setEditor] = useWorkspaceDraft(`strategy-editor:${destination}`, fresh);
  const [selected, setSelected] = useWorkspaceDraft<ReferenceStrategyPackage | null>(
    `strategy-selected:${destination}`,
    null,
  );
  const [items, setItems] = useState<ReferenceStrategyPackage[]>([]);
  const [after, setAfter] = useState("");
  const [evidence, setEvidence] = useState<Evidence | null>(null);
  const [loading, setLoading] = useState(false);
  const [confirmed, setConfirmed] = useState(false);
  const [reason, setReason] = useState("");
  const [drafts, setDrafts] = useState<ReferenceStrategyDelivery[]>([]);
  const [nextDraft, setNextDraft] = useState("");
  const scope = `${destination}:${selected?.object.id ?? ""}`;
  const [delivery, setDelivery] = useWorkspaceDraft<ReferenceStrategyDelivery | null>(
    `strategy-delivery:${scope}`,
    null,
  );
  const [deliveryIds, setDeliveryIds] = useWorkspaceDraft(`strategy-target:${scope}`, () => ({
    operation: `delivery-operation-${crypto.randomUUID()}`,
    id: `delivery-${crypto.randomUUID()}`,
    target: `backtest-${crypto.randomUUID()}`,
  }));
  const [accepted, setAccepted] = useState<boolean | null>(null);
  const [freezeId, setFreezeId] = useWorkspaceRequestId(
    `strategy-freeze:${destination}`,
    JSON.stringify([editor, evidence?.task.result_digest]),
  );
  const [retireId, setRetireId] = useWorkspaceRequestId(`strategy-retire:${scope}`, reason);
  const { error, setError, run } = useRun(trade);
  const ask = useRef(query);
  ask.current = query;
  const blocked = busy || loading || !service?.online;
  useWorkspaceUnsaved(
    `strategy-editor:${destination}`,
    !selected && Boolean(editor.name || editor.purpose || editor.limitations),
  );
  const list = useCallback(async (cursor = "") => {
    const value = (await ask.current("research.strategy.list", { after: cursor, limit: 30 }))
      .research_strategy;
    if (!value) throw new Error(t("策略响应缺失"));
    const rows = value.packages.filter(
      (item): item is ReferenceStrategyPackage => item.evidence_relation === "reference_only",
    );
    setItems(previous => (cursor ? [...previous, ...rows] : rows));
    setAfter(value.next_after);
  }, []);
  const listDrafts = useCallback(
    async (cursor = "") => {
      if (!selected) return;
      const value = (
        await ask.current("research.strategy.delivery.list", {
          package_id: selected.object.id,
          after: cursor,
          limit: 30,
        })
      ).research_strategy;
      if (!value) throw new Error(t("策略响应缺失"));
      const rows = value.deliveries.filter(
        (item): item is ReferenceStrategyDelivery => item.reference_result !== null,
      );
      setDrafts(previous => (cursor ? [...previous, ...rows] : rows));
      setNextDraft(value.next_after);
    },
    [selected],
  );
  useEffect(() => {
    setEvidence(null);
    setConfirmed(false);
    setAccepted(null);
    setItems([]);
    void list().catch(value => setError(asDisplayError(value)));
  }, [destination, list, setError]);
  useEffect(() => {
    setEvidence(null);
    setConfirmed(false);
    setAccepted(null);
    setDrafts([]);
    setNextDraft("");
    void listDrafts().catch(value => setError(asDisplayError(value)));
  }, [listDrafts, setError]);
  async function perform(action: () => Promise<void>) {
    setLoading(true);
    setError("");
    try {
      await action();
    } catch (value) {
      setError(asDisplayError(value));
    } finally {
      setLoading(false);
    }
  }
  async function loadEvidence() {
    setEvidence(null);
    setConfirmed(false);
    const id = selected?.reference_result.id ?? editor.source;
    const value = (await query("task.result.read", { owner: taskOwner(service, "research"), id }))
      .task_result;
    if (
      !value ||
      value.kind !== "backtest" ||
      value.id !== id ||
      value.task.retired ||
      (selected &&
        (value.task.result_digest !== selected.reference_result.version ||
          value.input_sha256 !== selected.source_input_sha256))
    )
      throw new Error(t("策略需要一致且未退役的正式回测来源"));
    setEvidence(value);
  }
  async function inspectDraft(id: string) {
    const result = (await query("research.strategy.delivery.read", { id })).research_strategy;
    if (!result?.delivery?.reference_result) throw new Error(t("策略响应缺失"));
    setDelivery(result.delivery);
    setAccepted(result.target_accepted ?? null);
    setConfirmed(false);
  }
  const sourceMatches =
    evidence &&
    evidence.id === (selected?.reference_result.id ?? editor.source) &&
    (!selected ||
      (evidence.task.result_digest === selected.reference_result.version &&
        evidence.input_sha256 === selected.source_input_sha256));
  const actual = service?.tasks.find(task => task.id === delivery?.target_task_id);
  return (
    <ResearchPage taskService={service} error={error}>
      <div className="inline-actions">
        <button
          disabled={blocked}
          onClick={() => {
            if (
              !selected &&
              (editor.name || editor.purpose || editor.limitations) &&
              !window.confirm(t("放弃当前未保存的研究草稿？"))
            )
              return;
            setSelected(null);
            setEditor(fresh());
            setEvidence(null);
            setConfirmed(false);
          }}
        >
          {t("新建策略候选")}
        </button>
        <button disabled={blocked} onClick={() => void perform(() => list())}>
          {t("刷新候选列表")}
        </button>
        <details className="subtle">
          <summary>{t("说明")}</summary>
          <p>
            {t(
              "候选保存原单合约或完整组合决策及参考回测；历史可得未知和事后参考限制仍保留，不证明预测有效，也不授权实盘。交付草稿不启动任务。",
            )}
          </p>
        </details>
      </div>
      {items.length > 0 && (
        <ul>
          {items.map(value => (
            <li key={value.object.id}>
              <button disabled={blocked} onClick={() => setSelected(value)}>
                {value.name} · {value.object.id}
              </button>
              {value.retirement_reason && (
                <span>
                  {t("已弃用")} · {value.retirement_reason}
                </span>
              )}
            </li>
          ))}
        </ul>
      )}
      {after && (
        <button disabled={blocked} onClick={() => void perform(() => list(after))}>
          {t("更多候选")}
        </button>
      )}
      {!selected && (
        <fieldset disabled={blocked}>
          <legend>{t("冻结正式回测中的固定决策定义")}</legend>
          {(
            [
              ["source", "来源回测任务"],
              ["name", "候选名称"],
              ["purpose", "候选用途"],
              ["limitations", "候选限制"],
            ] as const
          ).map(([key, label]) => (
            <label key={key}>
              {t(label)}
              <input
                value={editor[key]}
                onChange={event => {
                  setEditor({ ...editor, [key]: event.target.value });
                  if (key === "source") {
                    setEvidence(null);
                    setConfirmed(false);
                  }
                }}
              />
            </label>
          ))}
          <button disabled={!editor.source.trim()} onClick={() => void perform(loadEvidence)}>
            {t("读取固定回测假设")}
          </button>
          <button
            disabled={
              !sourceMatches ||
              !editor.name.trim() ||
              !editor.purpose.trim() ||
              !editor.limitations.trim() ||
              !service?.owner_node_id
            }
            onClick={() =>
              void perform(async () => {
                if (!evidence || !service?.owner_node_id) return;
                const id = freezeId ?? `freeze-${crypto.randomUUID()}`;
                setFreezeId(id);
                if (
                  !(await run("research.strategy.freeze", {
                    id,
                    registration: {
                      id: editor.id,
                      name: editor.name,
                      purpose: editor.purpose,
                      limitations: editor.limitations,
                      source: {
                        node: service.owner_node_id,
                        service: service.service,
                        domain: "research",
                        kind: "backtest_result",
                        id: evidence.id,
                        version: evidence.task.result_digest,
                      },
                    },
                  }))
                )
                  return;
                const value = (await query("research.strategy.read", { id: editor.id }))
                  .research_strategy?.package;
                if (!value || value.evidence_relation !== "reference_only")
                  throw new Error(t("策略响应缺失"));
                setSelected(value);
                setFreezeId(null);
                await list();
              })
            }
          >
            {t("冻结策略候选")}
          </button>
        </fieldset>
      )}
      {selected && (
        <section aria-label={t("固定候选定义")}>
          <h3>
            {selected.name} · {selected.object.id}
          </h3>
          <p>{selected.purpose}</p>
          <p>{selected.limitations}</p>
          <p>{t("原回测仅作参考，不是此包的已使用记录。")}</p>
          <dl>
            <dt>{t("合约")}</dt>
            <dd>
              {selected.definition.portfolio
                ? selected.definition.portfolio.contracts
                    .map(c => `${c.venue}/${c.symbol}`)
                    .join(" · ")
                : `${selected.definition.contract.venue}/${selected.definition.contract.symbol}`}
            </dd>
            <dt>{t(selected.definition.portfolio ? "固定组合政策" : "固定规则")}</dt>
            <dd>
              {selected.definition.portfolio
                ? strategyRule(selected.definition.portfolio.strategy)
                : strategyRule(selected.definition.strategy)}
            </dd>
            {selected.definition.portfolio &&
              strategyRows(selected.definition.portfolio.strategy).map(([label, value]) => (
                <Fragment key={label}>
                  <dt>{label}</dt>
                  <dd>{value}</dd>
                </Fragment>
              ))}
            <dt>{t("K线周期（分钟，0为日线）")}</dt>
            <dd>{selected.definition.interval_minutes}</dd>
            <dt>{t("定义身份")}</dt>
            <dd>{selected.object.version}</dd>
            <dt>{t("原执行程序")}</dt>
            <dd>{selected.definition.program_artifact}</dd>
            <dt>{t("原风险实现")}</dt>
            <dd>{selected.definition.risk_artifact}</dd>
          </dl>
          {selected.retirement_reason && (
            <p role="status">
              {t("已弃用")} · {selected.retirement_reason}
            </p>
          )}
          <button disabled={blocked} onClick={() => void perform(loadEvidence)}>
            {t("读取固定回测假设")}
          </button>
          <fieldset disabled={blocked || Boolean(selected.retirement_reason)}>
            <legend>{t("持久交付草稿")}</legend>
            <label>
              {t("目标回测任务ID")}
              <input
                value={deliveryIds.target}
                disabled={Boolean(delivery)}
                onChange={event => setDeliveryIds({ ...deliveryIds, target: event.target.value })}
              />
            </label>
            <button
              disabled={Boolean(delivery) || !deliveryIds.target.trim()}
              onClick={() =>
                void perform(async () => {
                  if (
                    !(await run("research.strategy.delivery.create", {
                      id: deliveryIds.operation,
                      delivery: {
                        id: deliveryIds.id,
                        package: selected.object,
                        target_task_id: deliveryIds.target,
                      },
                    }))
                  )
                    return;
                  await inspectDraft(deliveryIds.id);
                  await listDrafts();
                })
              }
            >
              {t("保存交付草稿，不运行")}
            </button>
          </fieldset>
          {delivery && (
            <button
              disabled={blocked || accepted === null || Boolean(selected.retirement_reason)}
              onClick={() => {
                setDelivery(null);
                setAccepted(null);
                setConfirmed(false);
                setDeliveryIds({
                  operation: `delivery-operation-${crypto.randomUUID()}`,
                  id: `delivery-${crypto.randomUUID()}`,
                  target: `backtest-${crypto.randomUUID()}`,
                });
              }}
            >
              {t("另建独立交付草稿")}
            </button>
          )}
          <button disabled={blocked} onClick={() => void perform(() => listDrafts())}>
            {t("刷新交付草稿")}
          </button>
          <ul>
            {drafts.map(value => (
              <li key={value.object.id}>
                <button
                  disabled={blocked}
                  onClick={() => void perform(() => inspectDraft(value.object.id))}
                >
                  {value.object.id} · {value.target_task_id}
                </button>
              </li>
            ))}
          </ul>
          {nextDraft && (
            <button disabled={blocked} onClick={() => void perform(() => listDrafts(nextDraft))}>
              {t("更多交付草稿")}
            </button>
          )}
          <label>
            {t("弃用原因")}
            <input
              value={reason}
              onChange={event => setReason(event.target.value)}
              disabled={blocked || Boolean(selected.retirement_reason)}
            />
          </label>
          <button
            disabled={blocked || !reason.trim() || Boolean(selected.retirement_reason)}
            onClick={() =>
              void perform(async () => {
                const id = retireId ?? `deprecate-${crypto.randomUUID()}`;
                setRetireId(id);
                if (
                  !(await run("research.strategy.deprecate", {
                    id,
                    package: selected.object,
                    reason,
                  }))
                )
                  return;
                const value = (await query("research.strategy.read", { id: selected.object.id }))
                  .research_strategy?.package;
                if (!value || value.evidence_relation !== "reference_only")
                  throw new Error(t("策略响应缺失"));
                setSelected(value);
                setRetireId(null);
                await list();
              })
            }
          >
            {t("弃用候选，保留既有任务")}
          </button>
        </section>
      )}
      {sourceMatches && evidence && (
        <section aria-label={t("固定交接假设")}>
          <h3>{t("固定交接假设")}</h3>
          <p>{t("沿用原数据版本、资金、逐日费用、滑点和风险；本次交接不能修改这些假设。")}</p>
          <ExperimentDetails evidence={evidence} />
        </section>
      )}
      {delivery && (
        <section aria-label={t("策略交付状态")}>
          <h3>{t("策略交付状态")}</h3>
          <p>
            {delivery.object.id} · {delivery.target_task_id}
          </p>
          <p role="status">
            {accepted === true
              ? t("Research已接纳；不代表任务成功")
              : accepted === false
                ? t("草稿已保存，尚无正式接纳")
                : t("接纳状态未确认，请查询原草稿")}
          </p>
          {actual && (
            <p>
              {t(states[actual.state])} · {actual.id}
            </p>
          )}
          <button
            disabled={blocked}
            onClick={() => void perform(() => inspectDraft(delivery.object.id))}
          >
            {t("查询原草稿与接纳状态")}
          </button>
          {selected?.definition.portfolio && (
            <>
              <p>
                {t(
                  "新配置锁定全部组合政策，需另行确认Data版本、资金、费用与风险；打开配置不接纳任务。",
                )}
              </p>
              <button
                disabled={blocked || accepted !== false || Boolean(selected.retirement_reason)}
                onClick={() =>
                  navigate("workspace.backtest", {
                    page: "backtest",
                    params: { strategy_delivery_id: delivery.object.id },
                  })
                }
              >
                {t("按固定组合新配置回测")}
              </button>
            </>
          )}
          <label>
            <input
              type="checkbox"
              checked={confirmed}
              disabled={
                blocked ||
                !sourceMatches ||
                accepted === true ||
                Boolean(selected?.retirement_reason)
              }
              onChange={event => setConfirmed(event.target.checked)}
            />
            {t("确认原固定回测假设，显式提交研究任务")}
          </label>
          <button
            disabled={
              blocked ||
              !confirmed ||
              !sourceMatches ||
              accepted === true ||
              Boolean(selected?.retirement_reason)
            }
            onClick={() =>
              void perform(async () => {
                if (
                  !(await run("backtest.reuse", {
                    id: delivery.target_task_id,
                    source_id: delivery.reference_result.id,
                    source_result_digest: delivery.reference_result.version,
                    source_input_sha256: delivery.source_input_sha256,
                    mode: "fixed",
                    snapshot: "",
                    strategy_use: { package: delivery.package, delivery: delivery.object },
                  }))
                )
                  return;
                await inspectDraft(delivery.object.id);
              })
            }
          >
            {t("显式提交固定回测")}
          </button>
          <button
            disabled={blocked || !accepted || actual?.state !== "succeeded"}
            onClick={async () => {
              if (
                await run("task.result", {
                  owner: taskOwner(service, "research"),
                  id: delivery.target_task_id,
                })
              )
                navigate("workspace.backtest", {
                  page: "backtest",
                  params: { result_id: delivery.target_task_id },
                });
            }}
          >
            {t("查看交付回测正式结果")}
          </button>
        </section>
      )}
    </ResearchPage>
  );
}
