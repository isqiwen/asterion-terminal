import { useState } from "react";
import { decimal, decimalText } from "../../src/bridge/decimal";
import type { LiveSnapshot } from "../../src/generated/asterion/trading_pb";
import type { TerminalContext } from "../contract";
import {
  command,
  key as contractKey,
  policyContract,
  riskLimits,
  t,
  type LimitsDraft,
  type Run,
} from "./live-shared";

const currentLimits = (live: LiveSnapshot): LimitsDraft => ({
  max_order_quantity: decimalText(live.risk?.maxOrderQuantity),
  max_gross_quantity: decimalText(live.risk?.maxGrossQuantity),
  max_working_orders: String(live.risk?.maxWorkingOrders ?? 0n),
  max_price_deviation: decimalText(live.maxPriceDeviation),
});

export function AccountPolicyEditor({
  live,
  snapshot,
  disabled,
  run,
}: {
  live: LiveSnapshot;
  snapshot: TerminalContext["snapshot"];
  disabled: boolean;
  run: Run;
}) {
  const [limits, setLimits] = useState(() => currentLimits(live));
  const [contracts, setContracts] = useState(live.contracts.map(contractKey));
  const [choice, setChoice] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  const catalog = snapshot?.market?.catalog;
  const available = catalog?.phase === "ready" ? catalog.contracts : [];
  return (
    <section aria-label={t("账户政策")}>
      <h3>{t("账户政策")}</h3>
      <p>
        {t("政策版本")}：<code>{live.policyRevision}</code>
      </p>
      <p className="subtle">
        {t(
          "保留历史政策与命令。现有持仓和在途委托仍计入风险；变更后断开连接，需重新登录、同步和授权。",
        )}
      </p>
      <form
        onSubmit={event => {
          event.preventDefault();
          // A contract already allowed keeps the terms it was recorded with.
          void command(run, live, () => ({
            case: "livePolicy",
            value: {
              policy: {
                risk: riskLimits(limits),
                maxPriceDeviation: decimal(limits.max_price_deviation),
                contracts: contracts.map(
                  value =>
                    live.contracts.find(item => contractKey(item) === value) ??
                    policyContract(available.find(item => contractKey(item) === value)!),
                ),
              },
            },
          })).then(applied => {
            if (applied) setConfirmed(false);
          });
        }}
      >
        <fieldset disabled={disabled || live.phase !== "ready"}>
          <div className="futures-fields">
            {(
              [
                ["max_order_quantity", "单笔数量上限"],
                ["max_gross_quantity", "总持仓量上限"],
                ["max_working_orders", "在途委托数上限"],
                ["max_price_deviation", "价格偏离上限"],
              ] as const
            ).map(([field, label]) => (
              <label key={field}>
                {t(label)}
                <input
                  aria-label={t(label)}
                  inputMode="decimal"
                  required
                  value={limits[field]}
                  onChange={event => setLimits({ ...limits, [field]: event.target.value })}
                />
              </label>
            ))}
          </div>
          <ul className="dataset-list" aria-label={t("政策授权合约")}>
            {contracts.map(value => (
              <li key={value}>
                {value}{" "}
                <button
                  type="button"
                  onClick={() => setContracts(contracts.filter(item => item !== value))}
                >
                  {t("移除")}
                </button>
              </li>
            ))}
          </ul>
          <label>
            {t("添加合约")}
            <select
              aria-label={t("政策新增合约")}
              value={choice}
              onChange={event => setChoice(event.target.value)}
            >
              <option value="">{t("请选择合约")}</option>
              {available
                .filter(item => !contracts.includes(contractKey(item)))
                .map(item => (
                  <option key={contractKey(item)} value={contractKey(item)}>
                    {contractKey(item)} · {item.name}
                  </option>
                ))}
            </select>
          </label>
          <button
            type="button"
            disabled={!choice || contracts.length >= 20}
            onClick={() => {
              setContracts([...contracts, choice]);
              setChoice("");
            }}
          >
            {t("添加")}
          </button>
          {catalog?.phase !== "ready" && (
            <p className="subtle">
              {t("新增合约前，请在行情页取得合约目录；现有合约仍可调整风险限额。")}
            </p>
          )}
          <label className="checkbox">
            <input
              type="checkbox"
              checked={confirmed}
              onChange={event => setConfirmed(event.target.checked)}
            />
            {t("我确认应用上述政策并重新登录")}
          </label>
          <button
            type="button"
            onClick={() => {
              setLimits(currentLimits(live));
              setContracts(live.contracts.map(contractKey));
              setChoice("");
              setConfirmed(false);
            }}
          >
            {t("恢复当前政策")}
          </button>
          <button className="primary" disabled={!confirmed || !contracts.length}>
            {t("应用新政策")}
          </button>
        </fieldset>
      </form>
    </section>
  );
}
