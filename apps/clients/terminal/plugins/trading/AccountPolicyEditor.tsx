import { useState } from "react";
import { translate, type TerminalContext } from "../contract";
import type { LiveSession, TerminalCommand } from "../../src/bridge/client";

const t = (key: string) => translate("asterion.terminal.trading", key);
const contractKey = (item: { venue: string; symbol: string }) => `${item.venue}.${item.symbol}`;

export function AccountPolicyEditor({
  live,
  snapshot,
  disabled,
  run,
}: {
  live: LiveSession;
  snapshot: TerminalContext["snapshot"];
  disabled: boolean;
  run: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<boolean>;
}) {
  const [limits, setLimits] = useState({
    ...live.risk,
    max_working_orders: String(live.risk.max_working_orders),
    max_price_deviation: live.max_price_deviation,
  });
  const [contracts, setContracts] = useState(live.contracts.map(contractKey));
  const [choice, setChoice] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  const catalog = snapshot?.market?.catalog;
  const available = catalog?.phase === "ready" ? catalog.contracts : [];
  return (
    <section aria-label={t("账户政策")}>
      <h3>{t("账户政策")}</h3>
      <p>
        {t("政策版本")}：<code>{live.policy_revision}</code>
      </p>
      <p>
        {t("风控算法摘要")}：<code>{live.risk_artifact}</code>
      </p>
      <p className="subtle">
        {t(
          "保留历史政策与命令。现有持仓和在途委托仍计入风险；变更后断开连接，需重新登录、同步和授权。",
        )}
      </p>
      <form
        onSubmit={event => {
          event.preventDefault();
          void run("live.policy.configure", {
            request_id: crypto.randomUUID(),
            account_id: live.account_id,
            policy_revision: live.policy_revision,
            risk_artifact: live.risk_artifact,
            ...limits,
            contracts: contracts.map(value => {
              const [venue, ...symbol] = value.split(".");
              return { venue, symbol: symbol.join(".") };
            }),
          }).then(applied => {
            if (applied) setConfirmed(false);
          });
        }}
      >
        <fieldset disabled={disabled || live.phase !== "ready" || live.storage_state !== "ready"}>
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
              setLimits({
                ...live.risk,
                max_working_orders: String(live.risk.max_working_orders),
                max_price_deviation: live.max_price_deviation,
              });
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
