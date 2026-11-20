import { FlowSteps } from "../../src/ui/FlowSteps";
import { useState } from "react";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { catalogName } from "../../src/ui/contract-names";
import type { TradingConnection } from "../../src/bridge/client";
import { decimal } from "../../src/bridge/decimal";
import type { Account } from "../../src/generated/asterion/trading_pb";
import { BackendError } from "../../src/i18n/errors";
import { LimitFields } from "./LimitFields";
import {
  t,
  environmentName,
  listedKey,
  optionalLimitFields,
  percentRatio,
  requiredLimitFields,
  limitText,
  policyEntryFromCatalog,
  policyOptionalFields,
  policyProducts,
  riskLimits,
  useTradingCatalog,
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
    max_daily_loss: "",
    max_drawdown: "",
    max_margin_ratio: "",
    max_daily_cancels_per_contract: "",
  });
  const [contracts, setContracts] = useWorkspaceDraft<string[]>(`live-contracts:${account.id}`, []);
  const [choice, setChoice] = useState("");
  const { contracts: listed, ready: catalogReady } = useTradingCatalog(account.id);
  const products = policyProducts(listed);
  return (
    <>
      <div role="status" aria-label={t("CTP 账户")}>
        <dl className="ctp-account-facts">
          {(
            [
              ["账户", account.instance],
              ["柜台环境", environment],
              ["经纪商", broker.brokerId],
              ["投资者", broker.userId],
              ["AppID", broker.appId],
              ["交易前置", broker.front],
            ] as const
          ).map(([label, value]) => (
            <div key={label}>
              <dt>{t(label)}</dt>
              <dd>{value}</dd>
            </div>
          ))}
        </dl>
      </div>
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
              maxPriceDeviation: decimal(percentRatio(limits.max_price_deviation)),
              ...policyOptionalFields(limits),
              contracts: contracts.map(value => {
                const entry = policyEntryFromCatalog(value, listed);
                if (!entry) throw new BackendError("unavailable", "market catalog not ready");
                return entry;
              }),
            },
          })).then(created => {
            if (created) setStep(0);
          });
        }}
      >
        <fieldset disabled={busy}>
          <fieldset hidden={step !== 0} disabled={step !== 0}>
            <section className="account-field-group" aria-label={t("可交易合约")}>
              <h3>{t("可交易合约")}</h3>
              {!catalogReady ? (
                <p role="status">{t("合约目录同步中")}</p>
              ) : (
                <div className="futures-file">
                  <label>
                    {t("合约")}
                    <input
                      aria-label={t("添加合约")}
                      list="live-catalog"
                      spellCheck={false}
                      autoCorrect="off"
                      autoCapitalize="none"
                      value={choice}
                      placeholder="SHFE.rb / SHFE.rb2610"
                      onChange={event => setChoice(event.target.value)}
                    />
                  </label>
                  <datalist id="live-catalog">
                    {products.map(({ key: id, sample }) => (
                      <option key={id} value={id}>
                        {[catalogName(sample), t("品种")].filter(Boolean).join(" · ")}
                      </option>
                    ))}
                    {listed.map(item => (
                      <option key={listedKey(item)} value={listedKey(item)}>
                        {catalogName(item)}
                      </option>
                    ))}
                  </datalist>
                  <button
                    type="button"
                    disabled={
                      !policyEntryFromCatalog(choice, listed) ||
                      contracts.includes(choice) ||
                      contracts.length >= 64
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
            <LimitFields limits={limits} change={setLimits} />
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
                  {[...requiredLimitFields, ...optionalLimitFields]
                    .filter(([field]) => (limits[field] ?? "").trim())
                    .map(([field, label, unit]) =>
                      limitText(t(label), (limits[field] ?? "").trim(), unit),
                    )
                    .join(" · ")}
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
