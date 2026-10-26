import { useCallback, useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type LogicDefinition,
  type LogicExample,
  type LogicExpectation,
  type LogicObservation,
  type TaskResult,
} from "../../src/bridge/client";
import {
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  type TerminalContext,
} from "../contract";
import { ResearchPage, t, useRun } from "./shared";
type Trial = Extract<TaskResult, { kind: "logic_trial" }>;
const fresh = () => ({
  id: `logic-${crypto.randomUUID()}`,
  name: "",
  purpose: "",
  fast: 1,
  slow: 3,
});
const exampleDraft = () => ({
  id: `case-${crypto.randomUUID()}`,
  name: "",
  scope: "",
  rows: "10,warmup\n10,warmup\n13,1\n7,-1",
});
function expectation(value: LogicExpectation) {
  return value.state === "warmup" ? t("预热") : String(value.value);
}
function observations(text: string): LogicObservation[] {
  const lines = text.trim().split(/\r?\n/);
  if (!text.trim() || lines.length > 512) throw new Error(t("样例需要1到512行"));
  return lines.map((line, index) => {
    const parts = line.split(",").map(value => value.trim());
    const price = parts[0] ?? "",
      sign = parts[1];
    if (
      parts.length !== 2 ||
      !/^[+-]?\d+(?:\.\d+)?$/.test(price) ||
      !["warmup", "-1", "0", "1"].includes(sign ?? "")
    )
      throw new Error(t("每行填写收盘值和预期，以逗号分隔"));
    const ns = String(BigInt(index + 1) * 60000000000n);
    const expected: LogicExpectation =
      sign === "warmup"
        ? { state: "warmup" }
        : { state: "target_sign", value: sign === "-1" ? -1 : sign === "0" ? 0 : 1 };
    return { closed_at_ns: ns, available_at_ns: ns, close: price, expected };
  });
}
export function ResearchLogic({ snapshot, query, trade, busy, navigate }: TerminalContext) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([
    service?.owner_node_id,
    service?.service,
    service?.connection_id,
  ]);
  const current = useRef(destination);
  current.current = destination;
  const ask = useRef(query);
  ask.current = query;
  const [editor, setEditor] = useWorkspaceDraft(`logic-editor:${destination}`, fresh);
  const [selected, setSelected] = useWorkspaceDraft<LogicDefinition | null>(
    `logic-selected:${destination}`,
    null,
  );
  const scope = `${destination}:${selected?.object.id ?? ""}`;
  const [draft, setDraft] = useWorkspaceDraft(`logic-case:${scope}`, exampleDraft);
  const [saved, setSaved] = useWorkspaceDraft<LogicExample | null>(`logic-prior:${scope}`, null);
  const [taskId, setTaskId] = useWorkspaceDraft(
    `logic-task:${scope}`,
    () => `logic-trial-${crypto.randomUUID()}`,
  );
  const [items, setItems] = useState<LogicDefinition[]>([]),
    [after, setAfter] = useState("");
  const [examples, setExamples] = useState<LogicExample[]>([]),
    [nextExample, setNextExample] = useState("");
  const [report, setReport] = useState<Trial | null>(null),
    [loading, setLoading] = useState(false),
    [reason, setReason] = useState("");
  const [registerId, setRegisterId] = useWorkspaceRequestId(
    `logic-register:${destination}`,
    JSON.stringify(editor),
  );
  const [saveId, setSaveId] = useWorkspaceRequestId(`logic-save:${scope}`, JSON.stringify(draft));
  const [retireId, setRetireId] = useWorkspaceRequestId(`logic-deprecate:${scope}`, reason);
  const { error, setError, run } = useRun(trade);
  const blocked = busy || loading || !service?.online;
  const list = useCallback(async (cursor = "") => {
    const owner = current.current;
    const value = (await ask.current("research.logic.list", { after: cursor, limit: 30 }))
      .research_logic;
    if (current.current !== owner) return;
    if (!value?.definitions) throw new Error(t("逻辑响应缺失"));
    setItems(previous => (cursor ? [...previous, ...value.definitions!] : value.definitions!));
    setAfter(value.next_after ?? "");
  }, []);
  const listExamples = useCallback(
    async (cursor = "") => {
      if (!selected) return;
      const owner = current.current;
      const value = (
        await ask.current("research.logic.examples", {
          definition_id: selected.object.id,
          after: cursor,
          limit: 30,
        })
      ).research_logic;
      if (current.current !== owner) return;
      if (!value?.examples) throw new Error(t("逻辑响应缺失"));
      setExamples(previous => (cursor ? [...previous, ...value.examples!] : value.examples!));
      setNextExample(value.next_after ?? "");
    },
    [selected],
  );
  useEffect(() => {
    setItems([]);
    setReport(null);
    void list().catch(e => setError(asDisplayError(e)));
  }, [destination, list, setError]);
  useEffect(() => {
    setExamples([]);
    setReport(null);
    void listExamples().catch(e => setError(asDisplayError(e)));
  }, [listExamples, setError]);
  async function action(work: () => Promise<void>) {
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
    const owner = current.current;
    const value = (await query("research.logic.read", { id })).research_logic?.definition;
    if (current.current !== owner) return;
    if (!value) throw new Error(t("逻辑响应缺失"));
    setSelected(value);
    setReport(null);
  }
  async function readResult() {
    if (!service || !saved) return;
    const owner = current.current;
    const value = (
      await query("task.result.read", { owner: taskOwner(service, "research"), id: taskId })
    ).task_result;
    if (current.current !== owner) return;
    if (
      !value ||
      value.kind !== "logic_trial" ||
      value.id !== taskId ||
      value.experiment.example.object.version !== saved.object.version ||
      value.experiment.example.object.id !== saved.object.id
    )
      throw new Error(t("试算结果与固定样例不一致"));
    setReport(value);
  }
  const task = service?.tasks.find(value => value.id === taskId);
  return (
    <ResearchPage taskService={service} error={error}>
      <p>{t("仅支持完成收盘上的双均线目标方向。虚构样例不进入Data，不证明收益或授权交易。")}</p>
      <button
        disabled={blocked}
        onClick={() => {
          setSelected(null);
          setEditor(fresh());
          setSaved(null);
          setReport(null);
        }}
      >
        {t("新建逻辑定义")}
      </button>
      <button disabled={blocked} onClick={() => void action(() => list())}>
        {t("刷新")}
      </button>
      <ul>
        {items.map(item => (
          <li key={item.object.id}>
            <button disabled={blocked} onClick={() => void action(() => inspect(item.object.id))}>
              {item.name}
            </button>{" "}
            {item.retirement_reason ? t("已停用") : t("可新建试算")}
          </li>
        ))}
      </ul>
      {after && (
        <button disabled={blocked} onClick={() => void action(() => list(after))}>
          {t("更多")}
        </button>
      )}
      {!selected ? (
        <fieldset disabled={blocked}>
          <legend>{t("独立规则定义")}</legend>
          <label>
            {t("名称")}
            <input
              value={editor.name}
              onChange={e => setEditor({ ...editor, name: e.target.value })}
            />
          </label>
          <label>
            {t("研究目的")}
            <input
              value={editor.purpose}
              onChange={e => setEditor({ ...editor, purpose: e.target.value })}
            />
          </label>
          <label>
            {t("快均线窗口")}
            <input
              type="number"
              min={1}
              max={9999}
              value={editor.fast}
              onChange={e => setEditor({ ...editor, fast: Number(e.target.value) })}
            />
          </label>
          <label>
            {t("慢均线窗口")}
            <input
              type="number"
              min={2}
              max={10000}
              value={editor.slow}
              onChange={e => setEditor({ ...editor, slow: Number(e.target.value) })}
            />
          </label>
          <button
            disabled={!editor.name.trim() || !editor.purpose.trim() || editor.fast >= editor.slow}
            onClick={() =>
              void action(async () => {
                const id = registerId ?? `logic-register-${crypto.randomUUID()}`;
                setRegisterId(id);
                if (await run("research.logic.register", { id, definition: editor })) {
                  await inspect(editor.id);
                  await list();
                }
              })
            }
          >
            {t("保存不可变规则")}
          </button>
        </fieldset>
      ) : (
        <>
          <h3>{selected.name}</h3>
          <p>{selected.purpose}</p>
          <p>
            {t("定义版本")} <code>{selected.object.version}</code> · {selected.fast}/{selected.slow}
          </p>
          <p>
            {t(
              "包含当前完成收盘；预热无方向，0为平仓目标方向。均值显示为近似数，方向按原精确算法确定。",
            )}
          </p>
          <label>
            {t("停用原因")}
            <input value={reason} onChange={e => setReason(e.target.value)} />
          </label>
          <button
            disabled={blocked || !reason.trim() || Boolean(selected.retirement_reason)}
            onClick={() =>
              void action(async () => {
                const id = retireId ?? `logic-deprecate-${crypto.randomUUID()}`;
                setRetireId(id);
                if (
                  await run("research.logic.deprecate", {
                    id,
                    definition: selected.object,
                    reason,
                  })
                ) {
                  await inspect(selected.object.id);
                  await list();
                }
              })
            }
          >
            {t("停用新试算，保留原证据")}
          </button>
          <ul>
            {examples.map(value => (
              <li key={value.object.id}>
                <button
                  disabled={blocked}
                  onClick={() => {
                    setSaved(value);
                    setTaskId(`logic-trial-${crypto.randomUUID()}`);
                    setReport(null);
                  }}
                >
                  {value.name}
                </button>
              </li>
            ))}
          </ul>
          {nextExample && (
            <button disabled={blocked} onClick={() => void action(() => listExamples(nextExample))}>
              {t("更多样例")}
            </button>
          )}
          <button
            disabled={blocked || Boolean(selected.retirement_reason)}
            onClick={() => {
              setSaved(null);
              setDraft(exampleDraft());
              setReport(null);
              setTaskId(`logic-trial-${crypto.randomUUID()}`);
            }}
          >
            {t("新建事先样例")}
          </button>
          {!saved ? (
            <fieldset disabled={blocked || Boolean(selected.retirement_reason)}>
              <legend>{t("先保存虚构观察及预期")}</legend>
              <label>
                {t("样例名称")}
                <input
                  value={draft.name}
                  onChange={e => setDraft({ ...draft, name: e.target.value })}
                />
              </label>
              <label>
                {t("适用范围")}
                <input
                  value={draft.scope}
                  onChange={e => setDraft({ ...draft, scope: e.target.value })}
                />
              </label>
              <p>{t("每行填写收盘值和预期，以逗号分隔")}</p>
              <p>
                {t(
                  "预期为warmup、-1、0或1；虚构边界从一分钟递增，可得时点等于该边界，最多512行。保存后不可覆盖预期。",
                )}
              </p>
              <label>
                {t("虚构收盘与事先预期")}
                <textarea
                  aria-label={t("虚构收盘与事先预期")}
                  rows={8}
                  value={draft.rows}
                  onChange={e => setDraft({ ...draft, rows: e.target.value })}
                />
              </label>
              <button
                disabled={!draft.name.trim() || !draft.scope.trim()}
                onClick={() =>
                  void action(async () => {
                    const captured = observations(draft.rows);
                    const id = saveId ?? `logic-case-save-${crypto.randomUUID()}`;
                    setSaveId(id);
                    if (
                      !(await run("research.logic.example.save", {
                        id,
                        example: {
                          id: draft.id,
                          definition: selected.object,
                          name: draft.name,
                          use_scope: draft.scope,
                          fictional: true,
                          observations: captured,
                        },
                      }))
                    )
                      return;
                    const value = (await query("research.logic.example.read", { id: draft.id }))
                      .research_logic?.example;
                    if (!value) throw new Error(t("逻辑响应缺失"));
                    setSaved(value);
                    await listExamples();
                  })
                }
              >
                {t("保存事先预期，不运行")}
              </button>
            </fieldset>
          ) : (
            <>
              {report && (
                <button
                  disabled={
                    blocked ||
                    !report.result.all_match ||
                    report.result.decisions === 0 ||
                    Boolean(selected.retirement_reason) ||
                    Boolean(report.task.retired)
                  }
                  onClick={() =>
                    navigate("workspace.strategy-packages", {
                      page: "strategies",
                      params: { logic_trial_id: report.id },
                    })
                  }
                >
                  {t("进入策略管理组装固定包")}
                </button>
              )}
              <h4>{saved.name}</h4>
              <p>{saved.use_scope}</p>
              <p>
                {t("固定样例版本")} <code>{saved.object.version}</code>
              </p>
              <label>
                {t("试算任务标识")}
                <input
                  value={taskId}
                  onChange={e => {
                    setTaskId(e.target.value);
                    setReport(null);
                  }}
                />
              </label>
              <button
                disabled={blocked || !taskId || Boolean(selected.retirement_reason)}
                onClick={() =>
                  void action(async () => {
                    await run("research.logic.trial", { id: taskId, example: saved.object });
                  })
                }
              >
                {t("运行固定样例试算")}
              </button>
              <span>{task?.state ?? t("尚未接纳")}</span>
              <button disabled={blocked || !taskId} onClick={() => void action(readResult)}>
                {t("读取原试算结果")}
              </button>
              <p>
                {t(
                  "预期不符是比较结果，不是执行失败；全部匹配也不证明策略有效。只有预热的样例未检验方向。",
                )}
              </p>
              {report && (
                <p>
                  {t("匹配行数")} {report.result.matched}/{report.result.rows.length} ·{" "}
                  {t("实际方向行数")} {report.result.decisions} · {t("原执行程序")}{" "}
                  <code>{report.task.program_artifact}</code>
                </p>
              )}
              <table>
                <thead>
                  <tr>
                    {[
                      "行",
                      "虚构收盘",
                      "完成/可得边界",
                      "事先预期",
                      "实际方向",
                      "快均值",
                      "慢均值",
                      "差值",
                      "比较",
                    ].map(value => (
                      <th key={value}>{t(value)}</th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {saved.observations.map((row, index) => {
                    const actual = report?.result.rows[index];
                    return (
                      <tr key={index}>
                        <td>{index + 1}</td>
                        <td>{row.close}</td>
                        <td>
                          {row.closed_at_ns} / {row.available_at_ns}
                        </td>
                        <td>{expectation(row.expected)}</td>
                        <td>{actual ? expectation(actual.actual) : "—"}</td>
                        <td>{actual?.fast_mean ?? "—"}</td>
                        <td>{actual?.slow_mean ?? "—"}</td>
                        <td>{actual?.difference ?? "—"}</td>
                        <td>{actual ? t(actual.matches ? "匹配" : "不匹配") : "—"}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </>
          )}
        </>
      )}
    </ResearchPage>
  );
}
