import { useEffect, useId, useState } from "react";
import { lab } from "../../src/bridge/lab";
import { RunState, type RunSummary } from "../../src/generated/asterion/lab_pb";
import { notify } from "../../src/host/notifications";
import {
  t,
  optionalLimitFields,
  ratioPercent,
  requiredLimitFields,
  type LimitsDraft,
} from "./live-shared";

// The limits a backtest ran with, as the form takes them; 0 there means none.
// A backtest has no price, loss, drawdown or cancel limits.
function backtestLimits(record: string): Partial<LimitsDraft> {
  const risk = JSON.parse(record).risk;
  const draft: Partial<LimitsDraft> = {};
  for (const [field, name] of [
    ["max_order_quantity", "max_order_lots"],
    ["max_contract_quantity", "max_contract_lots"],
    ["max_gross_quantity", "max_gross_lots"],
    ["max_working_orders", "max_working_orders"],
    ["max_margin_ratio", "max_margin_ratio"],
  ] as const) {
    const value = String(risk[name] ?? "0");
    if (value !== "0") draft[field] = field === "max_margin_ratio" ? ratioPercent(value) : value;
  }
  return draft;
}

const simulationPreset: Partial<LimitsDraft> = {
  max_order_quantity: "1",
  max_contract_quantity: "2",
  max_gross_quantity: "5",
  max_working_orders: "5",
  max_price_deviation: "2",
};

const positive = (raw: string | undefined) => {
  const text = (raw ?? "").trim();
  if (!text || !/^\d+(\.\d+)?$/.test(text)) return undefined;
  const n = Number(text);
  return Number.isFinite(n) && n > 0 ? n : undefined;
};

export function limitValidationErrors(
  limits: LimitsDraft,
): Partial<Record<keyof LimitsDraft, string>> {
  const errors: Partial<Record<keyof LimitsDraft, string>> = {};
  const order = positive(limits.max_order_quantity);
  const contract = positive(limits.max_contract_quantity);
  const gross = positive(limits.max_gross_quantity);
  if (contract !== undefined && gross !== undefined && contract > gross)
    errors.max_contract_quantity = t("单合约持仓上限不能大于账户总持仓上限");
  if (order !== undefined && contract !== undefined && order > contract)
    errors.max_order_quantity = t("单笔数量上限不能大于单合约持仓上限");
  return errors;
}

// The account's risk limits as a form: the five every account needs, then the
// optional capital and cancel limits, each with its unit.
export function LimitFields({
  limits,
  applied,
  change,
  offerSimPreset = false,
}: {
  limits: LimitsDraft;
  // What is in force, shown beside a field that differs from it.
  applied?: LimitsDraft;
  change: (next: LimitsDraft) => void;
  offerSimPreset?: boolean;
}) {
  const [runs, setRuns] = useState<RunSummary[]>([]);
  const [touched, setTouched] = useState<Partial<Record<keyof LimitsDraft, boolean>>>({});
  const errorBaseId = useId();
  useEffect(() => {
    // Without Lab the limits are entered by hand; the choice is not offered.
    void lab({ case: "listRecentRuns", value: {} })
      .then(reply =>
        setRuns(reply.runs.filter(run => run.state === RunState.SUCCEEDED && !run.sweepId)),
      )
      .catch(() => {});
  }, []);
  const bring = (id: string) =>
    void lab({ case: "getRun", value: { id } })
      .then(detail => change({ ...limits, ...backtestLimits(detail.record) }))
      .catch(failure => notify(failure, { namespace: "asterion.terminal.trading" }));
  const crossErrors = limitValidationErrors(limits);

  const field = (
    [name, label, unit]:
      (typeof requiredLimitFields)[number] | (typeof optionalLimitFields)[number],
    required: boolean,
  ) => {
    const raw = (limits[name] ?? "").trim();
    const isInt = name === "max_working_orders" || name === "max_daily_cancels_per_contract";
    const formatError =
      touched[name] && raw && (isInt ? !/^[1-9]\d*$/.test(raw) : positive(raw) === undefined)
        ? t("请输入大于 0 的有效数值")
        : undefined;
    const errorText = formatError ?? crossErrors[name];
    const errorId = `${errorBaseId}-${name}`;
    return (
      <label key={name} className="limit-field">
        <span className="limit-label">
          {t(label)}
          {applied && (applied[name] ?? "") !== (limits[name] ?? "") && (
            <small>{t("当前 {value}", { value: applied[name] || "—" })}</small>
          )}
        </span>
        <span className="unit-input">
          <input
            aria-label={t(label)}
            aria-invalid={errorText ? "true" : "false"}
            aria-describedby={errorText ? errorId : undefined}
            inputMode={isInt ? "numeric" : "decimal"}
            required={required}
            value={limits[name] ?? ""}
            onBlur={() => setTouched(current => ({ ...current, [name]: true }))}
            onChange={event => change({ ...limits, [name]: event.target.value })}
          />
          <span aria-hidden="true">{t(unit)}</span>
        </span>
        {errorText && (
          <span id={errorId} className="field-error" role="alert">
            {errorText}
          </span>
        )}
      </label>
    );
  };
  return (
    <>
      {(runs.length > 0 || offerSimPreset) && (
        <div className="limit-tools">
          {runs.length > 0 && (
            <label className="limit-source">
              {t("从回测带入")}
              <select value="" onChange={event => event.target.value && bring(event.target.value)}>
                <option value="">{t("选择回测")}</option>
                {runs.map(run => (
                  <option key={run.id} value={run.id}>
                    {run.label || `${run.strategy} ${run.firstDay}–${run.lastDay}`}
                  </option>
                ))}
              </select>
            </label>
          )}
          {offerSimPreset && (
            <button type="button" onClick={() => change({ ...limits, ...simulationPreset })}>
              {t("填入轻仓仿真参考")}
            </button>
          )}
        </div>
      )}
      <section className="account-field-group" aria-label={t("委托与持仓限制")}>
        <h3>{t("委托与持仓限制")}</h3>
        <div className="futures-fields">{requiredLimitFields.map(item => field(item, true))}</div>
      </section>
      <section className="account-field-group" aria-label={t("资金与撤单限制")}>
        <h3>
          {t("资金与撤单限制")} <small>{t("可选")}</small>
        </h3>
        <div className="futures-fields">{optionalLimitFields.map(item => field(item, false))}</div>
      </section>
    </>
  );
}
