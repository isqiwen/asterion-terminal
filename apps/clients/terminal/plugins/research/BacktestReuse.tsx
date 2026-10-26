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
import { retiredResearch } from "./shared";
type Evidence = Pick<
  Extract<TaskResult, { kind: "backtest" }>,
  "id" | "task" | "experiment" | "input_sha256"
>;
// Research varies one cost of a single-contract, full-sample daily momentum
// backtest on its original fixed data; other inputs have no cost variant.
function costVariable(value: Evidence) {
  const e = value.experiment;
  return (
    e.paper.contracts.length === 1 &&
    e.paper.contracts[0].dataset.interval_minutes === 0 &&
    !e.series?.length &&
    e.strategies.length === 1 &&
    e.strategies[0].rule.kind === "momentum" &&
    !e.holdout_day &&
    !e.walk_forward
  );
}
type Cost =
  { kind: "" } | { kind: "slippage"; ticks: string } | { kind: "fee"; multiplier: string };
export function BacktestReuse({
  evidence,
  snapshot,
  busy,
  query,
  run,
}: Pick<TerminalContext, "snapshot" | "busy" | "query"> & { evidence: Evidence; run: Run }) {
  const [mode, setMode] = useState<"" | "fixed" | "snapshot">("");
  const [selected, setSelected] = useState("");
  const [cost, setCost] = useState<Cost>({ kind: "" });
  const [error, setError] = useState<DisplayError>("");
  const [created, setCreated] = useState("");
  const connection = JSON.stringify([
    snapshot?.data?.connection_id,
    snapshot?.research_service?.connection_id,
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
      cost,
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
    setCost({ kind: "" });
    setError("");
    setCreated("");
  };
  async function create() {
    if (!mode) return;
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
        ...(cost.kind === "slippage" && { slippage_ticks: Number(cost.ticks) }),
        ...(cost.kind === "fee" && { fee_multiplier: cost.multiplier }),
      })
    ) {
      setCreated(id);
      setPending(null);
    }
  }
  if (retiredResearch(snapshot, evidence.task))
    return <p role="status">{t("原记录已退役，不能新增派生实验")}</p>;
  return (
    <section aria-label={t("按此回测创建")}>
      <h4>{t("按此回测创建")}</h4>
      <p className="subtle">
        {t(
          "复用经校验的原回测固定输入；同一研究服务内相同内容共享保存，任务与结果仍独立。保留策略、资金、逐日费用、风险、滑点及评价边界，不把重复留出当作新的样本外验证。",
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
            {costVariable(evidence) && (
              <fieldset>
                <legend>{t("成本变体（可选）")}</legend>
                <p className="subtle">
                  {t("只改变一项成本，其余固定输入不变；结果与原回测在比较中并列查看。")}
                </p>
                <select
                  aria-label={t("成本变体")}
                  value={cost.kind}
                  onChange={event => {
                    const kind = event.target.value;
                    setCost(
                      kind === "slippage"
                        ? { kind, ticks: "" }
                        : kind === "fee"
                          ? { kind, multiplier: "" }
                          : { kind: "" },
                    );
                  }}
                >
                  <option value="">{t("不改变成本")}</option>
                  <option value="slippage">{t("滑点跳数")}</option>
                  <option value="fee">{t("手续费倍数")}</option>
                </select>
                {cost.kind === "slippage" && (
                  <label>
                    {t("滑点跳数（0–100 的整数）")}
                    <input
                      inputMode="numeric"
                      value={cost.ticks}
                      onChange={event => setCost({ kind: "slippage", ticks: event.target.value })}
                    />
                  </label>
                )}
                {cost.kind === "fee" && (
                  <label>
                    {t("手续费倍数（0–100）")}
                    <input
                      inputMode="decimal"
                      value={cost.multiplier}
                      onChange={event => setCost({ kind: "fee", multiplier: event.target.value })}
                    />
                  </label>
                )}
              </fieldset>
            )}
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
            (cost.kind === "slippage" && !/^\d{1,3}$/.test(cost.ticks)) ||
            (cost.kind === "fee" && !/^\d+(\.\d+)?$/.test(cost.multiplier)) ||
            !snapshot?.data?.online ||
            !snapshot?.research_service?.online
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
