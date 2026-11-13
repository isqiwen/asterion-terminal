import { CatalogState } from "../../src/generated/asterion/market_pb";
import { useState } from "react";
import { useMarket } from "../../src/host/market";
import { decimal, decimalText } from "../../src/bridge/decimal";
import { BrokerState, type Account, type Policy } from "../../src/generated/asterion/trading_pb";
import { catalogName } from "../../src/ui/contract-names";
import {
  command,
  key as contractKey,
  listedKey,
  policyContract,
  riskLimits,
  t,
  type LimitsDraft,
  type Run,
} from "./live-shared";

const currentLimits = (policy: Policy | undefined): LimitsDraft => ({
  max_order_quantity: decimalText(policy?.limits?.maxOrderLots),
  max_gross_quantity: decimalText(policy?.limits?.maxGrossLots),
  max_contract_quantity: decimalText(policy?.limits?.maxContractLots),
  max_working_orders: String(policy?.limits?.maxWorkingOrders ?? 0),
  max_price_deviation: decimalText(policy?.maxPriceDeviation),
});

export function AccountPolicyEditor({
  record,
  disabled,
  run,
}: {
  record: Account;
  disabled: boolean;
  run: Run;
}) {
  const live = record.session!;
  const allowed = live.policy?.contracts ?? [];
  const [limits, setLimits] = useState(() => currentLimits(live.policy));
  const [contracts, setContracts] = useState(allowed.map(item => contractKey(item.instrument)));
  const [choice, setChoice] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  const catalog = useMarket()?.board.catalog;
  const available = catalog?.status?.state === CatalogState.READY ? catalog.contracts : [];
  return (
    <section aria-label={t("账户政策")}>
      <h3>{t("账户政策")}</h3>
      <p>
        {t("政策版本")}：<code>{live.policyRevision}</code>
      </p>
      <p className="subtle">{t("保存后断开柜台连接，需重新授权")}</p>
      <form
        onSubmit={event => {
          event.preventDefault();
          // A contract already allowed keeps the terms it was recorded with.
          void command(run, record, () => ({
            case: "changePolicy",
            value: {
              limits: riskLimits(limits),
              maxPriceDeviation: decimal(limits.max_price_deviation),
              contracts: contracts.map(
                value =>
                  allowed.find(item => contractKey(item.instrument) === value) ??
                  policyContract(available.find(item => listedKey(item) === value)!),
              ),
            },
          })).then(applied => {
            if (applied) setConfirmed(false);
          });
        }}
      >
        <fieldset disabled={disabled || live.brokerState !== BrokerState.READY}>
          <div className="futures-fields">
            {(
              [
                ["max_order_quantity", "单笔数量上限"],
                ["max_gross_quantity", "总持仓量上限"],
                ["max_contract_quantity", "单合约持仓上限"],
                ["max_working_orders", "在途委托数上限"],
                ["max_price_deviation", "价格偏离上限"],
              ] as const
            ).map(([field, label]) => (
              <label key={field}>
                {t(label)}
                <input
                  aria-label={t(label)}
                  inputMode="decimal"
                  placeholder={
                    field === "max_contract_quantity"
                      ? limits.max_gross_quantity || undefined
                      : undefined
                  }
                  required={field !== "max_contract_quantity"}
                  value={limits[field] ?? ""}
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
                .filter(item => !contracts.includes(listedKey(item)))
                .map(item => (
                  <option key={listedKey(item)} value={listedKey(item)}>
                    {[listedKey(item), catalogName(item)].filter(Boolean).join(" · ")}
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
          {catalog?.status?.state !== CatalogState.READY && (
            <p className="subtle">{t("合约目录未就绪，暂不能新增合约")}</p>
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
              setLimits(currentLimits(live.policy));
              setContracts(allowed.map(item => contractKey(item.instrument)));
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
