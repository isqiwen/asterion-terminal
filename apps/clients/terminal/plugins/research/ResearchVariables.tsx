import { useEffect, useRef, useState } from "react";
import type { ResearchVariableDefinition, ResearchVariablePreview } from "../../src/bridge/client";
import {
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type TerminalContext,
} from "../contract";
import { ResearchPage, t, useRun, variableUnit } from "./shared";
type Draft = { id: string; name: string; purpose: string; expression: string; window: string };
const fresh = (): Draft => ({
  id: `variable-${crypto.randomUUID()}`,
  name: "",
  purpose: "",
  expression: "close / lag(close, window) - 1",
  window: "2",
});
export function ResearchVariables({ snapshot, query, trade, busy, navigate }: TerminalContext) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([service?.node_id, service?.service]);
  const [draft, setDraft] = useWorkspaceDraft<Draft>(`variable-editor:${destination}`, fresh);
  const [saved, setSaved] = useWorkspaceDraft<ResearchVariableDefinition | null>(
    `variable-saved:${destination}`,
    null,
  );
  const [preview, setPreview] = useState<{
    fingerprint: string;
    value: ResearchVariablePreview;
  } | null>(null);
  const [items, setItems] = useState<ResearchVariableDefinition[]>([]);
  const [after, setAfter] = useState("");
  const [loading, setLoading] = useState(false);
  const { error, setError, run } = useRun(trade);
  const ask = useRef(query);
  ask.current = query;
  const window = Number(draft.window);
  const validWindow = /^\d+$/.test(draft.window) && window >= 1 && window <= 10000;
  const fingerprint = JSON.stringify([draft.expression, draft.window]);
  const checked = preview?.fingerprint === fingerprint ? preview.value : null;
  const dirty =
    !saved &&
    Boolean(
      draft.name ||
      draft.purpose ||
      draft.expression !== "close / lag(close, window) - 1" ||
      draft.window !== "2",
    );
  useWorkspaceUnsaved(`variable-editor:${destination}`, dirty);
  const [operation, setOperation] = useWorkspaceRequestId(
    `variable-register:${destination}`,
    JSON.stringify(draft),
  );
  const blocked = busy || loading || !service?.online;
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
  async function list(cursor = "") {
    const reply = (await ask.current("research.variable.list", { after: cursor, limit: 30 }))
      .research_variable;
    if (!reply) throw new Error(t("变量响应缺失"));
    setItems(previous => (cursor ? [...previous, ...reply.definitions] : reply.definitions));
    setAfter(reply.next_after);
  }
  useEffect(() => {
    let active = true;
    void (async () => {
      try {
        const reply = (await ask.current("research.variable.list", { after: "", limit: 30 }))
          .research_variable;
        if (!reply) throw new Error(t("变量响应缺失"));
        if (active) {
          setItems(reply.definitions);
          setAfter(reply.next_after);
        }
      } catch (reason) {
        if (active) setError(asDisplayError(reason));
      }
    })();
    return () => {
      active = false;
    };
  }, [destination, setError]);
  async function check() {
    const input = fingerprint;
    await perform(async () => {
      const reply = (
        await query("research.variable.check", { expression: draft.expression, window })
      ).research_variable?.preview;
      if (!reply) throw new Error(t("变量响应缺失"));
      setPreview({ fingerprint: input, value: reply });
    });
  }
  async function register() {
    const submitted = draft;
    const id = operation ?? `variable-register-${crypto.randomUUID()}`;
    setOperation(id);
    setLoading(true);
    try {
      if (
        !(await run("research.variable.register", { id, registration: { ...submitted, window } }))
      )
        return;
      const value = (await query("research.variable.read", { id: submitted.id })).research_variable
        ?.definition;
      if (
        !value ||
        value.object.id !== submitted.id ||
        value.name !== submitted.name ||
        value.purpose !== submitted.purpose ||
        value.expression !== submitted.expression ||
        value.window !== window
      )
        throw new Error(t("变量确认与原声明不一致"));
      setSaved(value);
      setOperation(null);
      await list();
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setLoading(false);
    }
  }
  const discard = () => !dirty || globalThis.window.confirm(t("放弃当前未保存的研究草稿？"));
  return (
    <ResearchPage title={t("研究变量定义")} taskService={service} error={error}>
      <p>
        {t(
          "定义固定因果表达式、用途与窗口；数据和未来标签在任务中单独选择。检查不读取行情，也不计算样本。",
        )}
      </p>
      <button
        disabled={blocked}
        onClick={() => {
          if (discard()) {
            setDraft(fresh());
            setSaved(null);
            setPreview(null);
          }
        }}
      >
        {t("新建变量定义")}
      </button>
      <fieldset disabled={blocked || Boolean(saved)}>
        <label>
          {t("变量名称")}
          <input
            value={draft.name}
            onChange={event => setDraft({ ...draft, name: event.target.value })}
          />
        </label>
        <label>
          {t("研究用途")}
          <textarea
            value={draft.purpose}
            onChange={event => setDraft({ ...draft, purpose: event.target.value })}
          />
        </label>
        <label>
          {t("表达式")}
          <textarea
            aria-label={t("表达式")}
            value={draft.expression}
            onChange={event => setDraft({ ...draft, expression: event.target.value })}
          />
        </label>
        <label>
          {t("固定窗口")}
          <input
            inputMode="numeric"
            value={draft.window}
            onChange={event => setDraft({ ...draft, window: event.target.value })}
          />
        </label>
        <button disabled={!validWindow} onClick={() => void check()}>
          {t("检查表达式")}
        </button>
        <button
          disabled={!validWindow || !draft.name.trim() || !draft.purpose.trim() || !checked?.valid}
          onClick={() => void register()}
        >
          {t("登记不可变定义")}
        </button>
      </fieldset>
      {checked && (
        <p role="status">
          {checked.valid
            ? `${t("输出单位")}: ${variableUnit(checked.price_power)} · ${t("实际预热观察数")}: ${checked.required_history}`
            : `${t("表达式检查未通过")}: ${checked.diagnostic} · ${t("字节位置")}: ${checked.error_offset ?? "—"}`}
        </p>
      )}
      {saved && (
        <section aria-label={t("原变量定义")}>
          <code>
            {saved.object.id} · {saved.object.version}
          </code>
          <p>
            {saved.name} · {saved.purpose}
          </p>
          <pre>{saved.expression}</pre>
          <p>
            {variableUnit(saved.price_power)} · {t("实际预热观察数")}: {saved.required_history}
          </p>
          <button
            disabled={blocked}
            onClick={() =>
              navigate("workspace.factor", {
                params: {
                  variable_id: saved.object.id,
                  variable_version: saved.object.version,
                  variable_node: saved.object.node,
                  variable_service: saved.object.service,
                },
              })
            }
          >
            {t("用于因子研究")}
          </button>
          <button
            disabled={blocked}
            onClick={() => {
              setDraft({ ...draft, id: `variable-${crypto.randomUUID()}` });
              setSaved(null);
              setPreview(null);
            }}
          >
            {t("基于此定义创建新草稿")}
          </button>
        </section>
      )}
      <h3>{t("已登记变量")}</h3>
      {items.map(item => (
        <button
          key={item.object.id}
          disabled={blocked}
          onClick={() => {
            if (discard()) {
              setSaved(item);
              setDraft({
                id: item.object.id,
                name: item.name,
                purpose: item.purpose,
                expression: item.expression,
                window: String(item.window),
              });
              setPreview(null);
            }
          }}
        >
          {item.name} · {item.object.id}
        </button>
      ))}
      {after && (
        <button disabled={blocked} onClick={() => void perform(() => list(after))}>
          {t("更多变量定义")}
        </button>
      )}
    </ResearchPage>
  );
}
