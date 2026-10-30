import { FlowSteps } from "../../src/ui/FlowSteps";
import { useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  useWorkspaceDraft,
  type TerminalContext,
} from "../contract";
import type { TradingConnection, TradingAccount } from "../../src/bridge/client";
import { t, key, type Run } from "./live-shared";

export function CreateLive({
  account,
  configuration,
  trade,
  snapshot,
  busy,
  run,
  openSettings,
}: Pick<TerminalContext, "snapshot" | "busy" | "openSettings" | "trade"> & {
  account: TradingConnection;
  configuration: TradingAccount;
  run: Run;
}) {
  // The record keeps its own copy of the account's counter details.
  const connection = {
    name: account.name,
    ...configuration.broker,
    trade_front: configuration.broker.front,
  };
  const environment = configuration.environment;
  const [step, setStep] = useWorkspaceDraft(`live-create-step:${account.id}`, 0);
  const [catalogError, setCatalogError] = useState<DisplayError>("");
  const [catalogCredentials, setCatalogCredentials] = useState({ password: "", auth_code: "" });
  const [limits, setLimits] = useWorkspaceDraft(`live-risk:${account.id}`, {
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
    max_price_deviation: "",
  });
  const [contracts, setContracts] = useWorkspaceDraft<string[]>(`live-contracts:${account.id}`, []);
  const [choice, setChoice] = useState("");
  const catalog = snapshot?.market?.catalog;
  const listed = catalog?.phase === "ready" ? catalog.contracts : [];
  return (
    <>
      <div className="inline-row">
        <p role="status" aria-label={t("CTP 账户")}>
          {`${connection.name} · ${configuration.account_id} · ${connection.app_id} · ${connection.broker_id} · ${connection.user_id} · ${
            connection.trade_front
          }`}
        </p>
        <button type="button" onClick={() => openSettings("trading")}>
          {t("管理交易连接")}
        </button>
      </div>
      <p className="subtle">{t("账户信息来自交易服务配置，只能在交易主机上设置。")}</p>
      <p className="subtle">{t("交易服务使用主机本地凭据登录柜台。")}</p>
      {!snapshot?.ctp_market && (
        <p>{t("请在行情设置中配置合约目录查询账户；它与交易服务账户独立。")}</p>
      )}
      {catalogError && <ErrorNotice error={catalogError} namespace="asterion.terminal.trading" />}
      <FlowSteps labels={[t("合约与风控"), t("确认创建")]} current={step} />
      <form
        onSubmit={event => {
          event.preventDefault();
          if (step === 0) {
            setStep(1);
            return;
          }
          if (!environment) return;
          void run("live.create", {
            ...limits,
            contracts: contracts.map(item => {
              const [venue, ...symbol] = item.split(".");
              return { venue, symbol: symbol.join(".") };
            }),
          }).then(created => {
            if (created) setStep(0);
          });
        }}
      >
        <fieldset disabled={busy}>
          <fieldset hidden={step !== 0} disabled={step !== 0}>
            <p>{t("账户与柜台环境由交易服务配置，Terminal 只配置风控。")}</p>
            <p>{t(environment === "real" ? "真实资金" : "CTP 仿真")}</p>
            <section className="account-field-group" aria-label={t("可交易合约")}>
              <h3>{t("可交易合约")}</h3>
              {catalog?.phase !== "ready" ? (
                <div>
                  <p className="subtle">
                    {t("先读取柜台合约规格，不会创建委托。凭据仅用于本次查询。")}
                  </p>
                  <div className="futures-fields">
                    <label>
                      {t("交易密码")}
                      <input
                        type="password"
                        autoComplete="off"
                        aria-label={t("目录查询密码")}
                        value={catalogCredentials.password}
                        onChange={e =>
                          setCatalogCredentials({ ...catalogCredentials, password: e.target.value })
                        }
                      />
                    </label>
                    <label>
                      {t("授权码")}
                      <input
                        type="password"
                        autoComplete="off"
                        aria-label={t("目录查询授权码")}
                        maxLength={16}
                        value={catalogCredentials.auth_code}
                        onChange={e =>
                          setCatalogCredentials({
                            ...catalogCredentials,
                            auth_code: e.target.value,
                          })
                        }
                      />
                    </label>
                  </div>
                  <button
                    type="button"
                    disabled={
                      busy ||
                      !snapshot?.ctp_market ||
                      !catalogCredentials.password ||
                      ["connecting", "authenticating", "logging_in", "querying"].includes(
                        catalog?.phase ?? "",
                      )
                    }
                    onClick={() => {
                      setCatalogError("");
                      const credentials = catalogCredentials;
                      setCatalogCredentials({ password: "", auth_code: "" });
                      if (snapshot?.ctp_market)
                        void trade("market.catalog", {
                          ...credentials,
                          account: snapshot.ctp_market,
                        }).catch(failure => setCatalogError(asDisplayError(failure)));
                    }}
                  >
                    {t("获取合约列表")}
                  </button>
                  <p role="status">
                    {t(
                      catalog?.phase === "error"
                        ? "合约查询失败，请核对连接信息后重试。"
                        : "合约规格未就绪",
                    )}
                  </p>
                </div>
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
                      <option key={key(item)} value={key(item)}>
                        {item.name}
                      </option>
                    ))}
                  </datalist>
                  <button
                    type="button"
                    disabled={
                      !listed.some(item => key(item) === choice) ||
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
                      placeholder={field === "max_price_deviation" ? "0.02" : undefined}
                      value={limits[field]}
                      onChange={event => setLimits({ ...limits, [field]: event.target.value })}
                      required
                    />
                  </label>
                ))}
              </div>
              <p className="subtle">
                {t(
                  "限价须在涨跌停范围内，且与券商最新价（当日无成交时为昨结算价）的偏离不超过该比例，例如 0.02 表示 2%。每笔委托发出前查询一次行情，约需 1 秒。",
                )}
              </p>
            </section>
          </fieldset>
          {step === 1 && (
            <section className="creation-review">
              <dl>
                <dt>{t("CTP 账户")}</dt>
                <dd>
                  <strong>{connection.name}</strong>
                  <span>
                    {connection.broker_id} · {connection.user_id}
                  </span>
                  <span>{connection.trade_front}</span>
                </dd>
                <dt>{t("交易服务连接")}</dt>
                <dd>{account.name}</dd>
                <dt>{t("柜台环境")}</dt>
                <dd>{t(environment === "real" ? "真实资金" : "CTP 仿真")}</dd>
                <dt>{t("可交易合约")}</dt>
                <dd>{contracts.join(" + ")}</dd>
                <dt>{t("委托与持仓限制")}</dt>
                <dd>
                  {t("单笔数量上限")} {limits.max_order_quantity} · {t("总持仓量上限")}{" "}
                  {limits.max_gross_quantity} · {t("在途委托数上限")} {limits.max_working_orders} ·{" "}
                  {t("价格偏离上限")} {limits.max_price_deviation}
                </dd>
              </dl>
              <p className="subtle">
                {t("创建账户不会登录或发送委托。下一步确认环境、连接并授权。")}
              </p>
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
