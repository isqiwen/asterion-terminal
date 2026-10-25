import { FlowSteps } from "../../src/ui/FlowSteps";
import { useState } from "react";
import { useWorkspaceDraft, type TerminalContext } from "../contract";
import type { CtpConnection } from "../../src/bridge/client";
import { t, key, type Run } from "./live-shared";

export function CreateLive({
  account,
  snapshot,
  busy,
  run,
  openSettings,
}: Pick<TerminalContext, "snapshot" | "busy" | "openSettings"> & {
  account: CtpConnection;
  run: Run;
}) {
  // The record keeps its own copy of the account's counter details.
  const connection = account;
  const [step, setStep] = useWorkspaceDraft(`live-create-step:${account.id}`, 0);
  const [catalogCredentials, setCatalogCredentials] = useState({ password: "", auth_code: "" });
  const [limits, setLimits] = useWorkspaceDraft(`live-risk:${account.id}`, {
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
    max_price_deviation: "",
  });
  const [contracts, setContracts] = useWorkspaceDraft<string[]>(`live-contracts:${account.id}`, []);
  const [choice, setChoice] = useState("");
  const [target, setTarget] = useWorkspaceDraft(`live-target:${account.id}`, {
    node: "local",
    service: "",
  });
  const placement = account.trading ?? target;
  const [environment, setEnvironment] = useWorkspaceDraft<"" | "real" | "simulation">(
    `live-create-environment:${account.id}`,
    "",
  );
  const nodes = snapshot?.nodes ?? [];
  const selectedNode = nodes.find(item => item.id === placement.node);
  const services = selectedNode?.health?.services.filter(item => item.kind === "live") ?? [];
  const catalog = snapshot?.market?.catalog;
  const listed = catalog?.phase === "ready" ? catalog.contracts : [];
  return (
    <>
      <div className="inline-row">
        <p role="status" aria-label={t("CTP 账户")}>
          {`${connection.name} · ${connection.broker_id} · ${connection.user_id} · ${
            connection.trade_front
          }`}
        </p>
        <button type="button" onClick={() => openSettings("ctp")}>
          {t("管理 CTP 账户")}
        </button>
      </div>
      <p className="subtle">{t("开通后柜台信息固定在此账户的交易记录中，不能再修改。")}</p>
      <p className="subtle">{t("密码与授权码在每次连接时输入，不保存。")}</p>
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
            ...placement,
            environment,
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
            <div className="futures-fields">
              <label>
                {t("运行节点")}
                <select
                  value={placement.node}
                  disabled={!!account.trading}
                  onChange={event => setTarget({ node: event.target.value, service: "" })}
                >
                  <option value="local">{t("本机")}</option>
                  {nodes
                    .filter(item => item.id !== "local")
                    .map(item => (
                      <option key={item.id} value={item.id}>
                        {item.id}
                      </option>
                    ))}
                  {account.trading && placement.node !== "local" && !selectedNode && (
                    <option value={placement.node}>{placement.node}</option>
                  )}
                </select>
              </label>
              <label>
                {t("交易服务实例")}
                <select
                  value={placement.service}
                  required={placement.node !== "local"}
                  disabled={!!account.trading}
                  onChange={event => setTarget({ ...target, service: event.target.value })}
                >
                  <option value="">
                    {t(placement.node === "local" ? "自动创建本机实例" : "请选择已部署的交易实例")}
                  </option>
                  {services.map(item => (
                    <option key={item.id} value={item.id}>
                      {item.id}
                    </option>
                  ))}
                  {account.trading && !services.some(item => item.id === placement.service) && (
                    <option value={placement.service}>{placement.service}</option>
                  )}
                </select>
              </label>
              <label>
                {t("柜台环境")}
                <select
                  required
                  value={environment}
                  onChange={event => setEnvironment(event.target.value as typeof environment)}
                >
                  <option value="">{t("请选择并核对")}</option>
                  <option value="simulation">{t("CTP 仿真")}</option>
                  <option value="real">{t("真实资金")}</option>
                </select>
              </label>
            </div>
            <p className="subtle">
              {t("远程实例请先在运行与部署中创建；绑定后不会自动切换节点。")}
            </p>
            <button type="button" onClick={() => openSettings("connections")}>
              {t("管理运行节点")}
            </button>
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
                      !catalogCredentials.password ||
                      ["connecting", "authenticating", "logging_in", "querying"].includes(
                        catalog?.phase ?? "",
                      )
                    }
                    onClick={() => {
                      const credentials = catalogCredentials;
                      setCatalogCredentials({ password: "", auth_code: "" });
                      void run("market.catalog", credentials);
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
                <dt>{t("运行节点")}</dt>
                <dd>
                  {placement.node === "local" ? t("本机") : placement.node} /{" "}
                  {placement.service || t("自动创建本机实例")}
                </dd>
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
