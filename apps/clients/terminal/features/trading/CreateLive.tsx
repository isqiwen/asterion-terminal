import { CatalogState } from "../../src/generated/asterion/market_pb";
import { FlowSteps } from "../../src/ui/FlowSteps";
import { useState } from "react";
import { useMarket } from "../../src/host/market";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { catalogName } from "../../src/ui/contract-names";
import type { TradingConnection } from "../../src/bridge/client";
import { decimal } from "../../src/bridge/decimal";
import type { Account } from "../../src/generated/asterion/trading_pb";
import {
  t,
  environmentName,
  listedKey,
  policyContract,
  riskLimits,
  type LimitsDraft,
  type Run,
} from "./live-shared";

export function CreateLive({
  account,
  configuration,
  busy,
  run,
}: Pick<TerminalContext, "busy"> & {
  account: TradingConnection;
  configuration: Account;
  run: Run;
}) {
  const broker = configuration.broker!;
  const environment = t(environmentName(configuration.environment));
  const [step, setStep] = useWorkspaceDraft(`live-create-step:${account.id}`, 0);
  const [limits, setLimits] = useWorkspaceDraft<LimitsDraft>(`live-risk:${account.id}`, {
    max_order_quantity: "",
    max_gross_quantity: "",
    max_contract_quantity: "",
    max_working_orders: "",
    max_price_deviation: "",
  });
  const [contracts, setContracts] = useWorkspaceDraft<string[]>(`live-contracts:${account.id}`, []);
  const [choice, setChoice] = useState("");
  const catalog = useMarket()?.board.catalog;
  const listed = catalog?.status?.state === CatalogState.READY ? catalog.contracts : [];
  return (
    <>
      <p role="status" aria-label={t("CTP 账户")}>
        {`${account.instance} · ${configuration.id} · ${broker.appId} · ${broker.brokerId} · ${broker.userId} · ${broker.front}`}
      </p>
      <FlowSteps labels={[t("合约与风控"), t("确认创建")]} current={step} />
      <form
        onSubmit={event => {
          event.preventDefault();
          if (step === 0) {
            setStep(1);
            return;
          }
          void run(() => ({
            case: "openAccount",
            value: {
              limits: riskLimits(limits),
              maxPriceDeviation: decimal(limits.max_price_deviation),
              contracts: contracts.map(value =>
                policyContract(listed.find(item => listedKey(item) === value)!),
              ),
            },
          })).then(created => {
            if (created) setStep(0);
          });
        }}
      >
        <fieldset disabled={busy}>
          <fieldset hidden={step !== 0} disabled={step !== 0}>
            <p>{environment}</p>
            <section className="account-field-group" aria-label={t("可交易合约")}>
              <h3>{t("可交易合约")}</h3>
              {catalog?.status?.state !== CatalogState.READY ? (
                <p role="status">{t("连接行情服务后可选择合约")}</p>
              ) : (
                <div className="futures-file">
                  <label>
                    {t("合约")}
                    <input
                      aria-label={t("添加合约")}
                      list="live-catalog"
                      value={choice}
                      placeholder="SHFE.rb2610"
                      onChange={event => setChoice(event.target.value)}
                    />
                  </label>
                  <datalist id="live-catalog">
                    {listed.map(item => (
                      <option key={listedKey(item)} value={listedKey(item)}>
                        {catalogName(item)}
                      </option>
                    ))}
                  </datalist>
                  <button
                    type="button"
                    disabled={
                      !listed.some(item => listedKey(item) === choice) ||
                      contracts.includes(choice) ||
                      contracts.length >= 20
                    }
                    onClick={() => {
                      setContracts([...contracts, choice]);
                      setChoice("");
                    }}
                  >
                    {t("添加")}
                  </button>
                </div>
              )}
              <ul className="dataset-list" aria-label={t("已选可交易合约")}>
                {contracts.map(item => (
                  <li key={item}>
                    <p>
                      <strong>{item}</strong>{" "}
                      <button
                        type="button"
                        aria-label={t("移除 {contract}", { contract: item })}
                        onClick={() => setContracts(contracts.filter(other => other !== item))}
                      >
                        {t("移除")}
                      </button>
                    </p>
                  </li>
                ))}
              </ul>
            </section>
            <section className="account-field-group" aria-label={t("委托与持仓限制")}>
              <h3>{t("委托与持仓限制")}</h3>
              <div className="futures-fields">
                {(
                  [
                    ["max_order_quantity", "单笔数量上限"],
                    ["max_contract_quantity", "单合约持仓上限"],
                    ["max_gross_quantity", "账户总持仓上限"],
                    ["max_working_orders", "在途委托数上限"],
                    ["max_price_deviation", "价格偏离上限"],
                  ] as const
                ).map(([field, label]) => (
                  <label key={field}>
                    {t(label)}
                    <input
                      aria-label={t(label)}
                      inputMode="decimal"
                      placeholder={field === "max_price_deviation" ? "0.02" : undefined}
                      value={limits[field]}
                      onChange={event => setLimits({ ...limits, [field]: event.target.value })}
                      required
                    />
                  </label>
                ))}
              </div>
            </section>
          </fieldset>
          {step === 1 && (
            <section className="creation-review">
              <dl>
                <dt>{t("CTP 账户")}</dt>
                <dd>
                  <strong>{account.instance}</strong>
                  <span>
                    {broker.brokerId} · {broker.userId}
                  </span>
                  <span>{broker.front}</span>
                </dd>
                <dt>{t("交易服务连接")}</dt>
                <dd>{account.instance}</dd>
                <dt>{t("柜台环境")}</dt>
                <dd>{environment}</dd>
                <dt>{t("可交易合约")}</dt>
                <dd>{contracts.join(" + ")}</dd>
                <dt>{t("委托与持仓限制")}</dt>
                <dd>
                  {t("单笔数量上限")} {limits.max_order_quantity} · {t("单合约持仓上限")}{" "}
                  {limits.max_contract_quantity} · {t("账户总持仓上限")} {limits.max_gross_quantity}{" "}
                  · {t("在途委托数上限")} {limits.max_working_orders} · {t("价格偏离上限")}{" "}
                  {limits.max_price_deviation}
                </dd>
              </dl>
            </section>
          )}
          <div className="workflow-actions">
            {step === 1 && (
              <button type="button" onClick={() => setStep(0)}>
                {t("上一步")}
              </button>
            )}
            <button className="primary" type="submit" disabled={!contracts.length}>
              {t(step === 1 ? "创建 CTP 账户" : "下一步")}
            </button>
          </div>
        </fieldset>
      </form>
    </>
  );
}
