import { useMemo, useState } from "react";
import { decimal, decimalText } from "../../src/bridge/decimal";
import { BrokerState, type Account, type Policy } from "../../src/generated/asterion/trading_pb";
import { BackendError } from "../../src/i18n/errors";
import { ContractPicker } from "./ContractPicker";
import { LimitFields, limitValidationErrors } from "./LimitFields";
import { catalogName } from "../../src/ui/contract-names";
import {
  command,
  key as contractKey,
  listedKey,
  percentRatio,
  ratioPercent,
  policyEntryFromCatalog,
  policyOptionalFields,
  policyProducts,
  riskLimits,
  t,
  useTradingCatalog,
  type LimitsDraft,
  type Run,
} from "./live-shared";

const currentLimits = (policy: Policy | undefined): LimitsDraft => ({
  max_order_quantity: decimalText(policy?.limits?.maxOrderLots),
  max_gross_quantity: decimalText(policy?.limits?.maxGrossLots),
  max_contract_quantity: decimalText(policy?.limits?.maxContractLots),
  max_working_orders: String(policy?.limits?.maxWorkingOrders ?? 0),
  max_price_deviation: ratioPercent(decimalText(policy?.maxPriceDeviation)),
  max_daily_loss: policy?.maxDailyLoss?.text ?? "",
  max_drawdown: policy?.maxDrawdown?.text ?? "",
  max_margin_ratio: policy?.maxMarginRatio?.text ? ratioPercent(policy.maxMarginRatio.text) : "",
  max_daily_cancels_per_contract: policy?.maxDailyCancelsPerContract
    ? String(policy.maxDailyCancelsPerContract)
    : "",
});

export function AccountPolicyEditor({
  connection,
  record,
  disabled,
  run,
}: {
  connection: string;
  record: Account;
  disabled: boolean;
  run: Run;
}) {
  const live = record.session!;
  const allowed = live.policy?.contracts ?? [];
  const [limits, setLimits] = useState(() => currentLimits(live.policy));
  const [contracts, setContracts] = useState(allowed.map(item => contractKey(item.instrument)));
  const [confirmed, setConfirmed] = useState(false);
  const { contracts: available, ready: catalogReady } = useTradingCatalog(connection);
  const selectable = useMemo(() => {
    const selected = new Set(contracts);
    const products = policyProducts(available);
    return [
      ...products
        .filter(({ key: id }) => !selected.has(id))
        .map(({ key: id, sample }) => ({
          id,
          label: [catalogName(sample), t("品种")].filter(Boolean).join(" · "),
          isProduct: true,
        })),
      ...available
        .filter(item => !selected.has(listedKey(item)))
        .map(item => ({
          id: listedKey(item),
          label: catalogName(item),
        })),
    ];
  }, [available, contracts]);
  const hasLimitErrors = Object.keys(limitValidationErrors(limits)).length > 0;
  return (
    <section className="policy-editor" aria-label={t("风控设置")}>
      <div className="workflow-heading">
        <h2>{t("风控设置")}</h2>
        <code title={live.policyRevision}>{live.policyRevision.slice(0, 8)}</code>
      </div>
      {live.brokerState !== BrokerState.READY && (
        <p role="status" className="subtle">
          {t("账户连接并同步后可修改风控设置")}
        </p>
      )}
      <form
        onSubmit={event => {
          event.preventDefault();
          if (!confirmed || !contracts.length || hasLimitErrors) return;
          // A contract already allowed keeps the terms it was recorded with.
          void command(run, record, () => ({
            case: "changePolicy",
            value: {
              limits: riskLimits(limits),
              maxPriceDeviation: decimal(percentRatio(limits.max_price_deviation)),
              ...policyOptionalFields(limits),
              contracts: contracts.map(value => {
                const existing = allowed.find(item => contractKey(item.instrument) === value);
                if (existing) return existing;
                const listed = policyEntryFromCatalog(value, available);
                if (!listed) throw new BackendError("unavailable", "market catalog not ready");
                return listed;
              }),
            },
          })).then(applied => {
            if (applied) setConfirmed(false);
          });
        }}
      >
        <fieldset disabled={disabled || live.brokerState !== BrokerState.READY}>
          <div className="policy-limits">
            <LimitFields
              limits={limits}
              applied={currentLimits(live.policy)}
              change={next => {
                setLimits(next);
                setConfirmed(false);
              }}
            />
          </div>
          <section className="policy-contracts">
            <h3>{t("可交易合约")}</h3>
            <ul aria-label={t("可交易合约")}>
              {contracts.map(value => (
                <li key={value}>
                  {value}
                  <button
                    type="button"
                    onClick={() => {
                      setContracts(contracts.filter(item => item !== value));
                      setConfirmed(false);
                    }}
                  >
                    {t("移除")}
                  </button>
                </li>
              ))}
            </ul>
            <div className="policy-add">
              <ContractPicker
                ariaLabel={t("新增可交易合约")}
                label={t("添加合约")}
                options={selectable}
                maxReached={contracts.length >= 64}
                onAdd={id => {
                  setContracts([...contracts, id]);
                  setConfirmed(false);
                }}
              />
              {!catalogReady && <p className="subtle">{t("合约目录未就绪，暂不能新增合约")}</p>}
            </div>
          </section>
          <footer className="policy-apply">
            <label className="checkbox">
              <input
                type="checkbox"
                checked={confirmed}
                onChange={event => setConfirmed(event.target.checked)}
              />
              {t("我确认应用上述风控设置并重新登录")}
            </label>
            <p className="subtle">{t("保存后断开柜台连接，需重新允许下单")}</p>
            <button
              type="button"
              onClick={() => {
                setLimits(currentLimits(live.policy));
                setContracts(allowed.map(item => contractKey(item.instrument)));
                setConfirmed(false);
              }}
            >
              {t("恢复当前设置")}
            </button>
            <button
              type="submit"
              className="primary"
              disabled={!confirmed || !contracts.length || hasLimitErrors}
            >
              {t("应用新风控设置")}
            </button>
          </footer>
        </fieldset>
      </form>
    </section>
  );
}
