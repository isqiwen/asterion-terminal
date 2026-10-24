import { useRef, useState } from "react";
import type { PositionSides, ResearchModelMapping, TaskResult } from "../../src/bridge/client";
import {
  asDisplayError,
  ErrorNotice,
  useWorkspaceRequestId,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { namespace, t, type Run } from "./shared";
type Evidence = Pick<
  Extract<TaskResult, { kind: "backtest" }>,
  "id" | "task" | "experiment" | "input_sha256"
>;
export function ModelReplaySetup({
  evidence,
  snapshot,
  query,
  busy,
  run,
}: Pick<TerminalContext, "snapshot" | "query" | "busy"> & { evidence: Evidence; run: Run }) {
  const owner = JSON.stringify([
    snapshot?.research_service?.owner_node_id,
    snapshot?.research_service?.service,
    evidence.id,
  ]);
  const current = useRef(owner);
  current.current = owner;
  const [listed, setListed] = useState<{
    owner: string;
    rows: ResearchModelMapping[];
    after: string;
  }>({ owner, rows: [], after: "" });
  const rows = listed.owner === owner ? listed.rows : [];
  const [selection, setSelection] = useState("");
  const [fold, setFold] = useState("");
  const [quantity, setQuantity] = useState("");
  const [sides, setSides] = useState<PositionSides | "">("");
  const [delay, setDelay] = useState("");
  const [budget, setBudget] = useState("");
  const [assumed, setAssumed] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [created, setCreated] = useState("");
  const selected = rows.find(row => row.object.id === selection);
  const integer = (value: string, low: number, high: number) =>
    /^\d+$/.test(value) &&
    Number.isSafeInteger(Number(value)) &&
    Number(value) >= low &&
    Number(value) <= high;
  const valid =
    selected &&
    integer(fold, 0, selected.folds.length - 1) &&
    integer(quantity, 1, 2147483647) &&
    ["both", "long", "short"].includes(sides) &&
    integer(delay, 1, 200000) &&
    integer(budget, 1, 3600) &&
    assumed;
  const [pending, setPending] = useWorkspaceRequestId(
    "model-account-replay",
    JSON.stringify([
      owner,
      evidence.task.result_digest,
      evidence.input_sha256,
      selected?.object,
      fold,
      quantity,
      sides,
      delay,
      budget,
      assumed,
    ]),
  );
  async function list(after = "") {
    const original = owner;
    try {
      const reply = (await query("research.model.list", { kind: "mapping", after, limit: 30 }))
        .research_model;
      if (current.current !== original) return;
      if (!reply) throw new Error(t("模型响应缺失"));
      const incoming = reply.mappings.filter(
        row => row.specification.model.kind === "model_time_validation_result",
      );
      setListed(old => ({
        owner: original,
        rows: after && old.owner === original ? [...old.rows, ...incoming] : incoming,
        after: reply.next_after,
      }));
      setError("");
    } catch (reason) {
      if (current.current === original) setError(asDisplayError(reason));
    }
  }
  async function create() {
    if (!valid || !selected || sides === "") return;
    const original = owner;
    const id = pending ?? `model-account-${crypto.randomUUID()}`;
    setPending(id);
    const ok = await run("backtest.reuse", {
      id,
      source_id: evidence.id,
      source_result_digest: evidence.task.result_digest,
      source_input_sha256: evidence.input_sha256,
      mode: "fixed",
      snapshot: "",
      model_replay: {
        mapping: selected.object,
        fold_index: Number(fold),
        quantity,
        sides,
        processing_delay_bars: Number(delay),
        time_budget_seconds: Number(budget),
        availability: "assume_original_bar_close",
      },
    });
    if (current.current !== original) return;
    if (ok) {
      setCreated(id);
      setPending(null);
    }
  }
  if (
    evidence.experiment.model_replay ||
    evidence.experiment.paper.contracts.length !== 1 ||
    evidence.experiment.paper.contracts[0].dataset.interval_minutes !== 1 ||
    evidence.experiment.series?.length ||
    evidence.experiment.walk_forward ||
    evidence.experiment.holdout_day
  )
    return null;
  return (
    <section aria-label={t("固定行情模型模拟")}>
      <h4>{t("固定行情模型模拟")}</h4>
      <p>
        {t(
          "沿用原行情、资金、逐日成本与风控；用固定原折重新按时钟训练，模型可用后才决策，后续行情才成交。历史可得时间未知，仅作明确假设下的离线研究。",
        )}
      </p>
      <fieldset disabled={busy || !snapshot?.research_service?.online}>
        <button onClick={() => void list()}>{t("读取固定逐折映射")}</button>
        {listed.owner === owner && listed.after && (
          <button onClick={() => void list(listed.after)}>{t("继续读取映射")}</button>
        )}
        <label>
          {t("固定逐折映射")}
          <select
            aria-label={t("固定逐折映射")}
            value={selection}
            onChange={event => {
              setSelection(event.target.value);
              setFold("");
            }}
          >
            <option value="">—</option>
            {rows.map(row => (
              <option key={row.object.id} value={row.object.id}>
                {row.specification.name} · {row.object.id}
              </option>
            ))}
          </select>
        </label>
        {selected && (
          <>
            <p>
              {selected.specification.purpose} · <code>{selected.object.version}</code>
            </p>
            <label>
              {t("模拟原折")}
              <select
                aria-label={t("模拟原折")}
                value={fold}
                onChange={event => setFold(event.target.value)}
              >
                <option value="">—</option>
                {selected.folds.map((row, index) => (
                  <option key={index} value={String(index)}>
                    {index + 1}: [{row.training_begin}, {row.training_end}) → [
                    {row.validation_begin}, {row.validation_end})
                  </option>
                ))}
              </select>
            </label>
          </>
        )}
        <label>
          {t("目标手数")}
          <input
            inputMode="numeric"
            value={quantity}
            onChange={event => setQuantity(event.target.value)}
          />
        </label>
        <label>
          {t("允许方向")}
          <select
            aria-label={t("允许方向")}
            value={sides}
            onChange={event => {
              const value = event.target.value;
              if (value === "" || value === "both" || value === "long" || value === "short")
                setSides(value);
            }}
          >
            <option value="">—</option>
            <option value="both">{t("双向")}</option>
            <option value="long">{t("只做多")}</option>
            <option value="short">{t("只做空")}</option>
          </select>
        </label>
        <label>
          {t("模型处理延迟（bar）")}
          <input
            inputMode="numeric"
            value={delay}
            onChange={event => setDelay(event.target.value)}
          />
        </label>
        <label>
          {t("模拟时间预算（秒）")}
          <input
            inputMode="numeric"
            value={budget}
            onChange={event => setBudget(event.target.value)}
          />
        </label>
        <label>
          <input
            type="checkbox"
            checked={assumed}
            onChange={event => setAssumed(event.target.checked)}
          />
          {t("我明确假设原完整bar在其收盘可用；此模拟不是历史可交易或实盘授权证据。")}
        </label>
        <button disabled={!valid} onClick={() => void create()}>
          {pending ? t("确认提交状态") : t("创建模型模拟任务")}
        </button>
      </fieldset>
      {error && <ErrorNotice error={error} namespace={namespace} />}
      {created && (
        <p role="status">
          {t("模型模拟任务已接纳")} <code>{created}</code>
        </p>
      )}
    </section>
  );
}
