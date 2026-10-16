import { useEffect, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceRequestId,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { TaskResult } from "../../src/bridge/client";
import { t, namespace, type Run } from "./shared";
type Evidence = Pick<Extract<TaskResult, { kind: "factor" }>, "id" | "task" | "experiment">;
export function FactorReuse({
  evidence,
  snapshot,
  busy,
  query,
  run,
}: Pick<TerminalContext, "snapshot" | "busy" | "query"> & { evidence: Evidence; run: Run }) {
  const [editing, setEditing] = useState(false);
  const [reviewed, setReviewed] = useState(false);
  const originalWindow = String(evidence.experiment.lookbacks[0]);
  const [lookback, setLookback] = useState(originalWindow);
  const canEdit =
    evidence.experiment.factor === "momentum" &&
    evidence.experiment.evaluation.mode === "full_sample" &&
    evidence.experiment.lookbacks.length === 1;
  const validWindow =
    Number.isInteger(Number(lookback)) && Number(lookback) >= 1 && Number(lookback) <= 10000;
  const [mode, setMode] = useState<"" | "fixed" | "snapshot">("");
  const [selected, setSelected] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [created, setCreated] = useState("");
  const connection = JSON.stringify([
    snapshot?.data?.connection_id,
    snapshot?.task_service?.connection_id,
  ]);
  const [pending, setPending] = useWorkspaceRequestId(
    "factor-reuse",
    JSON.stringify([
      connection,
      evidence.id,
      evidence.task.result_digest,
      evidence.experiment,
      mode,
      selected,
      editing,
      lookback,
    ]),
  );
  useEffect(() => {
    setEditing(false);
    setReviewed(false);
    setLookback(originalWindow);
    setMode("");
    setSelected("");
    setCreated("");
  }, [connection, evidence.id, evidence.task.result_digest, originalWindow]);
  useEffect(() => {
    if (mode !== "snapshot") return;
    setSelected("");
    let active = true;
    void query("data.tables.coverage", {})
      .then(reply => {
        if (!active) return;
        const value = reply.table_coverage?.snapshot ?? "";
        setSelected(value);
        if (!value) setError(t("当前数据服务没有可选择的新快照。"));
      })
      .catch(reason => {
        if (active) setError(asDisplayError(reason));
      });
    return () => {
      active = false;
    };
  }, [mode, connection, query]);
  const tables = evidence.experiment.series.filter(series => series.kind === "table");
  if (tables.length !== evidence.experiment.series.length)
    return (
      <p className="subtle">
        {t("当前实验复用仅支持全市场日线表因子；其他输入请在设置中显式创建新实验。")}
      </p>
    );
  const choose = (value: "fixed" | "snapshot") => {
    setReviewed(false);
    setMode(value);
    setSelected("");
    setError("");
    setCreated("");
  };
  async function create() {
    if (!mode) return;
    const id = pending ?? `factor-${crypto.randomUUID()}`;
    setPending(id);
    if (
      await run("factor.reuse", {
        id,
        source_id: evidence.id,
        source_result_digest: evidence.task.result_digest,
        experiment: evidence.experiment,
        mode,
        snapshot: mode === "fixed" ? "" : selected,
        ...(editing ? { lookbacks: [Number(lookback)] } : {}),
      })
    ) {
      setCreated(id);
      setPending(null);
      if (editing) {
        setEditing(false);
        setReviewed(false);
        setMode("");
        setLookback(originalWindow);
      }
    }
  }
  return (
    <section aria-label={t("按此日线实验创建")}>
      <h4>{t("按此日线实验创建")}</h4>
      <p className="subtle">
        {t(
          "复制当前兼容实验的因子、窗口、评价边界及筛选规则，创建独立新任务。原任务不变；重复使用同一留出区间不等于新的样本外验证。",
        )}
      </p>
      <p>
        {t("原任务")}: <code>{evidence.id}</code>
      </p>
      {canEdit && !editing && (
        <button
          disabled={busy || !!pending}
          onClick={() => {
            setEditing(true);
            setCreated("");
            setReviewed(false);
          }}
        >
          {t("修改回看窗口并派生")}
        </button>
      )}
      {editing && (
        <div>
          <p>
            {t(
              "仅修改单个回看日线数；因子、未来收益跨度、评价边界、标的范围和资格筛选保持继承。全样本比较不能作为新的样本外证明。",
            )}
          </p>
          <label>
            {t("派生回看日线数")}
            <input
              type="number"
              min="1"
              max="10000"
              value={lookback}
              disabled={busy || !!pending}
              onChange={event => {
                setLookback(event.target.value);
                setReviewed(false);
                setCreated("");
              }}
            />
          </label>
          <p>
            {t("回看窗口改动")}：{originalWindow} → {lookback || "—"}
          </p>
          <button
            disabled={busy || !!pending}
            onClick={() => {
              setEditing(false);
              setReviewed(false);
              setLookback(originalWindow);
              setMode("");
              setSelected("");
              setError("");
            }}
          >
            {t("取消参数派生")}
          </button>
        </div>
      )}
      <fieldset disabled={busy}>
        <legend>{t("新实验的数据选择")}</legend>
        <label>
          <input
            type="radio"
            name={`reuse-${evidence.id}`}
            disabled={!!pending}
            checked={mode === "fixed"}
            onChange={() => choose("fixed")}
          />
          {t("沿用原固定快照")}
        </label>
        <label>
          <input
            type="radio"
            name={`reuse-${evidence.id}`}
            disabled={!!pending}
            checked={mode === "snapshot"}
            onChange={() => choose("snapshot")}
          />
          {t("选择当前服务的新快照")}
        </label>
        {mode === "fixed" && (
          <>
            <p className="subtle">
              {t(
                "按原快照和范围重新读取，数据内容摘要必须与原实验一致；缺失或不一致时拒绝，不换用当前数据。",
              )}
            </p>
            {[...new Set(tables.map(series => series.data.snapshot))].map(value => (
              <p key={value}>
                <code>{value}</code>
              </p>
            ))}
          </>
        )}
        {mode === "snapshot" && (
          <>
            <p className="subtle">
              {t(
                "保留原来源和范围，固定下方已选择快照；数据或修订可能改变。提交后不会随最新快照移动，也不会自动下载缺失数据。",
              )}
            </p>
            <p>
              {t("新快照")}: <code>{selected || "—"}</code>
            </p>
          </>
        )}
        {editing && (
          <>
            <button
              disabled={
                !mode ||
                (mode === "snapshot" && !selected) ||
                !validWindow ||
                Number(lookback) === Number(originalWindow) ||
                !!pending
              }
              onClick={() => setReviewed(true)}
            >
              {t("核对参数派生")}
            </button>
            {reviewed && (
              <section aria-label={t("参数派生确认")}>
                <h5>{t("参数派生确认")}</h5>
                <p>
                  {t("回看窗口改动")}：{originalWindow} → {lookback}
                </p>
                <p>{t("继承的数据来源和范围")}</p>
                {tables.map(series => (
                  <p key={series.data.instrument}>
                    {series.data.instrument} · {series.data.daily_source} ·{" "}
                    {series.data.calendar_source} · {series.data.begin_day} → {series.data.end_day}
                  </p>
                ))}
                <p>
                  {t("原实验数据内容摘要")}：<code>{evidence.experiment.dataset_revision}</code>
                </p>
                <p>
                  {t(
                    "确认后创建独立任务，原参数和原结果不变；完成后可在日线因子比较中选择原任务与新任务。",
                  )}
                </p>
              </section>
            )}
          </>
        )}
        <button
          disabled={
            (editing && (!reviewed || !validWindow)) ||
            !mode ||
            (mode === "snapshot" && !selected) ||
            !snapshot?.data?.online ||
            !snapshot?.task_service?.online
          }
          onClick={() => void create()}
        >
          {pending
            ? t("确认提交状态")
            : editing
              ? t("确认创建派生因子任务")
              : t("创建独立因子任务")}
        </button>
      </fieldset>
      {error && <ErrorNotice error={error} namespace={namespace} />}
      {created && (
        <p role="status">
          {t("新任务已创建，原实验保留不变。")}: <code>{created}</code>
        </p>
      )}
    </section>
  );
}
