import { useEffect, useRef, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { NoticeRecord, NoticeSetRequest, NoticeSetView } from "../../src/bridge/client";
import { NoticeConsumers } from "./NoticeConsumers";
const t = (key: string) => translate("asterion.terminal.intelligence", key);
export function NoticeSets({ context }: { context: TerminalContext }) {
  const owner = context.snapshot?.intelligence_service;
  const [id, setId] = useState("");
  const [name, setName] = useState("");
  const [scope, setScope] = useState("research");
  const [cutoff, setCutoff] = useState(new Date().toISOString().slice(0, 23));
  const [mode, setMode] = useState<NoticeSetRequest["query"]["use_mode"]>("post_hoc_research");
  const [candidateId, setCandidateId] = useState("");
  const [rows, setRows] = useState<NoticeRecord[]>([]);
  const [selected, setSelected] = useState<NoticeRecord[]>([]);
  const [view, setView] = useState<NoticeSetView | null>(null);
  const [attempt, setAttempt] = useState<NoticeSetRequest | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  useEffect(() => {
    const token = epoch.current;
    ++token.value;
    setRows([]);
    setSelected([]);
    setView(null);
    setAttempt(null);
    setError("");
    setPending(false);
    return () => {
      ++token.value;
    };
  }, [owner?.connection_id]);
  const locked = pending || !!attempt;
  const milliseconds = Date.parse(cutoff + "Z");
  const location = selected[0]?.published?.content;
  const sameLocation =
    !!location &&
    selected.every(
      r =>
        r.published?.content.node === location.node &&
        r.published.content.service === location.service,
    );

  const valid =
    !!id &&
    !!name.trim() &&
    !!scope.trim() &&
    selected.length > 0 &&
    selected.length <= 16 &&
    Number.isSafeInteger(milliseconds) &&
    milliseconds > 0 &&
    milliseconds <= Date.now() &&
    sameLocation;
  function edit(work: () => void) {
    setView(null);
    work();
  }
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
  function accept(value: NoticeSetView | undefined) {
    if (!value) throw Error(t("集合响应缺失"));
    setView(value);
    if (value.state === "prepared") setAttempt(value.request);
  }
  const preview = () =>
    run(async current => {
      const request: NoticeSetRequest = {
        id,
        name,
        program_artifact: "",
        data_owner: { node: location!.node, service: location!.service, domain: "data" },
        query: {
          events: selected
            .map(r => r.event!.reference)
            .sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0)),
          as_of_ms: String(milliseconds),
          use_mode: mode,
          use_scope: scope,
        },
      };
      const reply = await context.query("intelligence.set.preview", { request });
      if (current === epoch.current.value) accept(reply.notice_set);
    });
  const publish = () =>
    run(async current => {
      const request = attempt ?? view!.request;
      setAttempt(request);
      await context.trade("intelligence.set.publish", { request });
      if (current !== epoch.current.value) return;
      const reply = await context.query("intelligence.set.get", { id: request.id });
      if (current === epoch.current.value) {
        accept(reply.notice_set);
        setAttempt(null);
      }
    });
  return (
    <section className="research-workspace" aria-label={t("固定事件集合")}>
      <h2>{t("固定事件集合")}</h2>
      <p>{t("仅覆盖明确选择的事件，不代表完整市场历史；固定截止、用途、成员和排除证据。")}</p>
      <p>
        {t("集合保存位置（单次对象选择，非部署绑定）")}：{location?.node}/{location?.service}
      </p>
      {selected.length > 0 && !sameLocation && (
        <p role="alert">
          {t("所选内容跨多个Data保存位置，当前不支持传输；不能丢弃成员后假称完整集合。")}
        </p>
      )}
      <button
        disabled={pending || !owner}
        onClick={() =>
          void run(async current => {
            const reply = await context.query("intelligence.list", { after: "" });
            if (current === epoch.current.value) setRows(reply.intelligence_records ?? []);
          })
        }
      >
        {t("读取前32条候选")}
      </button>
      <p>{t("列表只覆盖当前页；其它候选可按原ID读取，不把未列出视为没有事件。")}</p>
      <label>
        {t("候选 ID")}
        <input
          value={candidateId}
          disabled={pending}
          onChange={e => setCandidateId(e.target.value)}
        />
      </label>
      <button
        disabled={pending || !owner || !candidateId}
        onClick={() =>
          void run(async current => {
            const reply = await context.query("intelligence.get", { id: candidateId });
            if (current === epoch.current.value && reply.intelligence_record)
              setRows([reply.intelligence_record]);
          })
        }
      >
        {t("读取候选")}
      </button>
      <ul>
        {rows
          .filter(r => r.event && r.published)
          .filter(
            (r, i, all) =>
              all.findIndex(v => v.event!.reference.id === r.event!.reference.id) === i,
          )
          .map(r => {
            const event = r.event!.reference;
            const checked = selected.some(e => e.event!.reference.id === event.id);
            return (
              <li key={event.id}>
                <label>
                  <input
                    type="checkbox"
                    checked={checked}
                    disabled={locked || (!checked && selected.length >= 16)}
                    onChange={e =>
                      edit(() =>
                        setSelected(
                          e.target.checked
                            ? [...selected, r]
                            : selected.filter(v => v.event!.reference.id !== event.id),
                        ),
                      )
                    }
                  />
                  {r.contract} <code>{event.id}</code>
                </label>
              </li>
            );
          })}
      </ul>
      <p>
        {t("已选事件数")}：{selected.length}/16
      </p>
      <label>
        {t("集合操作 ID")}
        <input value={id} disabled={locked} onChange={e => edit(() => setId(e.target.value))} />
      </label>
      <button
        disabled={pending}
        onClick={() => {
          setId(crypto.randomUUID());
          setAttempt(null);
          setView(null);
        }}
      >
        {t("新建集合 ID")}
      </button>
      <label>
        {t("集合名称")}
        <input value={name} disabled={locked} onChange={e => edit(() => setName(e.target.value))} />
      </label>
      <label>
        {t("使用范围")}
        <input
          value={scope}
          disabled={locked}
          onChange={e => edit(() => setScope(e.target.value))}
        />
      </label>
      <label>
        {t("集合截止时间（UTC）")}
        <input
          type="datetime-local"
          step="0.001"
          value={cutoff}
          disabled={locked}
          onChange={e => edit(() => setCutoff(e.target.value))}
        />
      </label>
      <label>
        {t("集合研究模式")}
        <select
          value={mode}
          disabled={locked}
          onChange={e => edit(() => setMode(e.target.value as typeof mode))}
        >
          <option value="post_hoc_research">{t("事后研究")}</option>
          <option value="strict_history">{t("严格实际历史")}</option>
        </select>
      </label>
      <button disabled={locked || !owner || !valid} onClick={() => void preview()}>
        {t("预览固定选择与排除")}
      </button>
      <button
        disabled={pending || !owner || !id}
        onClick={() =>
          void run(async current => {
            const reply = await context.query("intelligence.set.get", { id });
            if (current === epoch.current.value) accept(reply.notice_set);
          })
        }
      >
        {t("核实原集合操作")}
      </button>
      {view && (
        <section aria-label={t("集合预览或固定结果")}>
          <h3>{view.request.name}</h3>
          <p>
            {t("状态")}：
            {t(
              { preview: "集合预览", prepared: "集合准备待保存", published: "集合已固定发布" }[
                view.state
              ],
            )}
          </p>
          <p>
            {t("使用范围")}：{view.request.query.use_scope} · {view.request.query.use_mode}
          </p>
          <p>
            {t("固定截止毫秒")}：{view.request.query.as_of_ms}
          </p>
          <p>
            {t("实际方法程序")}：<code>{view.request.program_artifact}</code>
          </p>
          <p>
            {t("实际组装可用时间")}：{view.available_at_ms ?? t("尚未发布")}
          </p>
          <p>{t("集合组装时间不代表集合在历史截止时已存在。")}</p>
          <h4>{t("固定成员")}</h4>
          <ul>
            {view.members.map(m => (
              <li key={m.event.event.reference.id}>
                <code>{m.event.event.reference.id}</code> · <code>{m.publication.version}</code>
                <ul>
                  {m.limitations.map((r, i) => (
                    <li key={i}>
                      {r.reason} · <code>{r.source.id}</code>
                    </li>
                  ))}
                </ul>
              </li>
            ))}
          </ul>
          <h4>{t("排除证据")}</h4>
          <ul>
            {view.exclusions.map(e => (
              <li key={e.event.id}>
                <code>{e.event.id}</code>
                <ul>
                  {e.reasons.map((r, i) => (
                    <li key={i}>
                      {r.reason} · <code>{r.source.id}</code>
                    </li>
                  ))}
                </ul>
              </li>
            ))}
          </ul>
          <button
            disabled={pending || view.state === "published" || !view.members.length}
            onClick={() => void publish()}
          >
            {t(attempt ? "重试原集合保存" : "确认保存并发布集合")}
          </button>
          {view.state === "published" && view.reference && (
            <>
              <code>{view.reference.version}</code>
              <button
                onClick={() =>
                  context.navigate("workspace.event-set-review", {
                    params: {
                      source: JSON.stringify(view.reference),
                      scope: view.request.query.use_scope,
                    },
                  })
                }
              >
                {t("交给 Research 集合审阅")}
              </button>
              <NoticeConsumers context={context} source={view.reference} />
            </>
          )}
        </section>
      )}
      <ErrorNotice error={error} />
    </section>
  );
}
