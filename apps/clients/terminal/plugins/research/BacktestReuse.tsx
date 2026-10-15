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
type Evidence = Pick<
  Extract<TaskResult, { kind: "backtest" }>,
  "id" | "task" | "experiment" | "input_sha256"
>;
export function BacktestReuse({
  evidence,
  snapshot,
  busy,
  query,
  run,
}: Pick<TerminalContext, "snapshot" | "busy" | "query"> & { evidence: Evidence; run: Run }) {
  const [mode, setMode] = useState<"" | "fixed" | "snapshot">("");
  const [selected, setSelected] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [created, setCreated] = useState("");
  const connection = JSON.stringify([
    snapshot?.data?.connection_id,
    snapshot?.task_service?.connection_id,
  ]);
  const [pending, setPending] = useWorkspaceRequestId(
    "backtest-reuse",
    JSON.stringify([
      connection,
      evidence.id,
      evidence.task.result_digest,
      evidence.input_sha256,
      mode,
      selected,
    ]),
  );
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
  const choose = (value: "fixed" | "snapshot") => {
    setMode(value);
    setSelected("");
    setError("");
    setCreated("");
  };
  async function create() {
    const id = pending ?? `backtest-${crypto.randomUUID()}`;
    setPending(id);
    if (
      await run("backtest.reuse", {
        id,
        source_id: evidence.id,
        source_result_digest: evidence.task.result_digest,
        source_input_sha256: evidence.input_sha256,
        mode,
        snapshot: mode === "fixed" ? "" : selected,
      })
    ) {
      setCreated(id);
      setPending(null);
    }
  }
  return (
    <section aria-label={t("按此回测创建")}>
      <h4>{t("按此回测创建")}</h4>
      <p className="subtle">
        {t(
          "复制经校验的原回测完整输入，保留策略、资金、逐日费用、风险、滑点及评价边界，创建独立新任务；不改变原实验，也不将重复留出当作新的样本外验证。",
        )}
      </p>
      <p>
        {t("原任务")}: <code>{evidence.id}</code>
      </p>
      <fieldset disabled={busy}>
        <legend>{t("新实验的数据选择")}</legend>
        <label>
          <input
            type="radio"
            name={`reuse-${evidence.id}`}
            checked={mode === "fixed"}
            onChange={() => choose("fixed")}
          />
          {t("沿用原固定快照")}
        </label>
        <label>
          <input
            type="radio"
            name={`reuse-${evidence.id}`}
            checked={mode === "snapshot"}
            onChange={() => choose("snapshot")}
          />
          {t("选择当前服务的新快照")}
        </label>
        {mode === "fixed" && (
          <>
            <p className="subtle">
              {t(
                "重新读取原固定数据并要求完整输入逐字节一致，包含费用、风险和换月证据；缺失或不一致即拒绝，不补默认值。",
              )}
            </p>
            {[
              ...new Set(
                evidence.experiment.paper.contracts.map(item => item.dataset.daily.snapshot),
              ),
            ].map(value => (
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
                "新快照更新日线及结算等表事实；分钟版本、原实际月份集合、费用和风险保持，换月计划按原首交易日重新解析。合约规格改变或日历证据不足时拒绝；不自动加入新月份或下载数据。",
              )}
            </p>
            <p>
              {t("新快照")}: <code>{selected || "—"}</code>
            </p>
          </>
        )}
        <button
          disabled={
            !mode ||
            (mode === "snapshot" && !selected) ||
            !snapshot?.data?.online ||
            !snapshot?.task_service?.online
          }
          onClick={() => void create()}
        >
          {pending ? t("确认提交状态") : t("创建独立回测任务")}
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
