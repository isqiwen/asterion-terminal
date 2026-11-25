import { FlowSteps } from "../../src/ui/FlowSteps";
import { useMemo } from "react";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { catalogName } from "../../src/ui/contract-names";
import type { TradingConnection } from "../../src/bridge/client";
import { decimal } from "../../src/bridge/decimal";
import { Environment, type Account } from "../../src/generated/asterion/trading_pb";
import { BackendError, ErrorNotice } from "../../src/i18n/errors";
import { Tag } from "../../src/ui/Tag";
import { ContractPicker } from "./ContractPicker";
import { LimitFields, limitValidationErrors } from "./LimitFields";
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
  const real = configuration.environment === Environment.REAL;
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
  const {
    contracts: listed,
    ready: catalogReady,
    error: catalogError,
    retry: retryCatalog,
  } = useTradingCatalog(account.id);
  const selectable = useMemo(() => {
    const chosen = new Set(contracts);
    const products = policyProducts(listed);
    return [
      ...products
        .filter(({ key: id }) => !chosen.has(id))
        .map(({ key: id, sample }) => ({
          id,
          label: [catalogName(sample), t("品种")].filter(Boolean).join(" · "),
          isProduct: true,
        })),
      ...listed
        .filter(item => !chosen.has(listedKey(item)))
        .map(item => ({
          id: listedKey(item),
          label: catalogName(item),
        })),
    ];
  }, [listed, contracts]);
  const hasLimitErrors = Object.keys(limitValidationErrors(limits)).length > 0;
  return (
    <>
      {real && (
        <div className="ctp-identity" data-real>
          <strong>
            {account.instance} · {broker.brokerId} · {broker.userId}
          </strong>
          <Tag danger>{environment}</Tag>
        </div>
      )}
      <div className="account-setup-meta" role="status" aria-label={t("CTP 账户")}>
        <dl className="ctp-account-facts">
          {(
            [
              ["账户", account.instance, false],
              ["柜台环境", environment, false],
              ["经纪商", broker.brokerId, true],
              ["投资者", broker.userId, true],
              ["AppID", broker.appId, true],
              ["交易前置", broker.front, true],
            ] as const
          ).map(([label, value, mono]) => (
            <div key={label}>
              <dt>{t(label)}</dt>
              <dd>
                {label === "柜台环境" ? (
                  <Tag danger={real}>{value}</Tag>
                ) : mono ? (
                  <code>{value}</code>
                ) : (
                  value
                )}
              </dd>
            </div>
          ))}
        </dl>
      </div>
      <FlowSteps labels={[t("合约与风控"), t("确认创建")]} current={step} />
      <form
        onSubmit={event => {
          event.preventDefault();
          if (hasLimitErrors) return;
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
              <header className="account-field-group-header">
                <h3>
                  {t("可交易合约")} <small>{t("必填")}</small>
                </h3>
              </header>
              {catalogReady ? (
                <ContractPicker
                  ariaLabel={t("添加合约")}
                  label={t("合约")}
                  options={selectable}
                  maxReached={contracts.length >= 64}
                  onAdd={id => setContracts([...contracts, id])}
                />
              ) : catalogError ? (
                <div className="catalog-load-error" role="alert">
                  <ErrorNotice error={catalogError} namespace="asterion.terminal.trading" />
                  <button type="button" onClick={retryCatalog}>
                    {t("重试")}
                  </button>
                </div>
              ) : (
                <p role="status" className="subtle">
                  {t("合约目录同步中")}
                </p>
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
            <LimitFields limits={limits} change={setLimits} offerSimPreset={!real} />
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
            <button
              className="primary"
              type="submit"
              disabled={!contracts.length || hasLimitErrors}
            >
              {t(step === 1 ? "创建 CTP 账户" : "下一步")}
            </button>
          </div>
        </fieldset>
      </form>
    </>
  );
}
