import { useState } from "react";
import type { LiveSession } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  saveCostTemplate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { AccountPolicyEditor } from "./AccountPolicyEditor";
import { ActivityTabs, type Activity } from "./ActivityTabs";
import { key, offsets, statuses, t, type Run } from "./live-shared";

// The account's positions, orders, fills, rates and risk policy, one tab at a
// time. A working order can be cancelled from its row.
export function AccountRecords({
  live,
  snapshot,
  busy,
  run,
  activity,
  setActivity,
  ready,
  stale,
  cancelling,
  cancel,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  live: LiveSession;
  run: Run;
  activity: Activity;
  setActivity: (value: Activity) => void;
  ready: boolean;
  stale: boolean;
  cancelling: readonly string[];
  cancel: (id: string) => Promise<void>;
}) {
  return (
    <>
      <ActivityTabs value={activity} onChange={setActivity} />
      <div hidden={activity !== "positions"}>
        <div className="paper-table" role="region" aria-label={t("CTP 持仓")} tabIndex={0}>
          <table aria-label={t("CTP 持仓")}>
            <thead>
              <tr>
                <th>{t("合约")}</th>
                <th>{t("方向")}</th>
                <th>{t("今仓")}</th>
                <th>{t("昨仓")}</th>
              </tr>
            </thead>
            <tbody>
              {live.positions.map(p => (
                <tr key={`${key(p)}.${p.side}`}>
                  <td>{p.symbol}</td>
                  <td>{p.side === "buy" ? t("多头") : t("空头")}</td>
                  <td>{p.today}</td>
                  <td>{p.yesterday}</td>
                </tr>
              ))}
            </tbody>
          </table>
          {!live.positions.length && <p className="subtle">{t("暂无持仓")}</p>}
        </div>
      </div>
      <div hidden={activity !== "orders"}>
        <div className="paper-table" role="region" aria-label={t("CTP 委托记录")} tabIndex={0}>
          <table aria-label={t("CTP 委托记录")}>
            <thead>
              <tr>
                <th>{t("合约")}</th>
                <th>{t("方向 / 开平")}</th>
                <th>{t("限价")}</th>
                <th>{t("手数")}</th>
                <th>{t("已成交")}</th>
                <th>{t("状态")}</th>
                <th>{t("操作")}</th>
              </tr>
            </thead>
            <tbody>
              {live.orders
                .slice()
                .reverse()
                .map(o => (
                  <tr key={o.broker_key}>
                    <td>{o.symbol}</td>
                    <td>
                      {o.side === "buy" ? t("买入") : t("卖出")} / {t(offsets[o.offset])}
                    </td>
                    <td>{o.limit_price}</td>
                    <td>{o.quantity}</td>
                    <td>{o.filled}</td>
                    <td>
                      {t(statuses[o.status])}
                      {o.error_code ? ` (${o.error_code})` : ""}
                    </td>
                    <td>
                      {o.id && ["submitted", "accepted", "partially_filled"].includes(o.status) && (
                        <button
                          disabled={
                            cancelling.includes(o.id) ||
                            stale ||
                            !(ready || (live.phase === "error" && live.error_code === -1008))
                          }
                          onClick={() => void cancel(o.id)}
                        >
                          {t("撤单")}
                        </button>
                      )}
                    </td>
                  </tr>
                ))}
            </tbody>
          </table>
          {!live.orders.length && <p className="subtle">{t("暂无委托")}</p>}
        </div>
      </div>
      <div hidden={activity !== "fills"}>
        <div className="paper-table" role="region" aria-label={t("CTP 成交")} tabIndex={0}>
          <table aria-label={t("CTP 成交")}>
            <thead>
              <tr>
                <th>{t("合约")}</th>
                <th>{t("时间")}</th>
                <th>{t("价格")}</th>
                <th>{t("手数")}</th>
              </tr>
            </thead>
            <tbody>
              {live.trades
                .slice()
                .reverse()
                .map(f => (
                  <tr key={f.id}>
                    <td>{f.symbol}</td>
                    <td>{f.trade_time}</td>
                    <td>{f.price}</td>
                    <td>{f.quantity}</td>
                  </tr>
                ))}
            </tbody>
          </table>
          {!live.trades.length && <p className="subtle">{t("暂无成交")}</p>}
        </div>
      </div>
      <div hidden={activity !== "rates"}>
        {ready ? (
          <AccountRates live={live} busy={busy} run={run} />
        ) : (
          <p className="subtle">{t("连接柜台后可查询账户费率。")}</p>
        )}
      </div>
      <div hidden={activity !== "policy"}>
        <section aria-label={t("风险限制")}>
          <h3>{t("风险限制")}</h3>
          <p>
            {t("单笔数量上限")}: {live.risk.max_order_quantity} · {t("总持仓量上限")}:{" "}
            {live.risk.max_gross_quantity} · {t("在途委托数上限")}: {live.risk.max_working_orders} ·{" "}
            {t("价格偏离上限")}: {live.max_price_deviation}
          </p>
          <p>
            {t("可交易合约")}: {live.contracts.map(item => key(item)).join("、")}
          </p>
        </section>
        <AccountPolicyEditor
          key={live.policy_revision}
          live={live}
          snapshot={snapshot}
          disabled={busy || stale}
          run={run}
        />
      </div>
    </>
  );
}

// The account's margin and commission rates as the broker reports them. A
// row can become the product's fee template for paper trading and backtests.
function AccountRates({ live, busy, run }: { live: LiveSession; busy: boolean; run: Run }) {
  const [saved, setSaved] = useState<string[]>([]);
  const [error, setError] = useState<DisplayError | null>(null);
  const day = live.trading_day.replace(/^(\d{4})(\d{2})(\d{2})$/, "$1-$2-$3");
  const states: Record<LiveSession["costs"][number]["state"], string> = {
    querying: "查询中",
    ready: "已返回",
    unavailable: "券商未提供",
  };
  return (
    <section className="account-rates" aria-label={t("账户费率")}>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      <div className="source-actions">
        <button disabled={busy} onClick={() => void run("live.costs")}>
          {t("查询账户费率")}
        </button>
        <span className="subtle">
          {t("券商对本账户实际收取的保证金率与手续费；多空保证金不同时取较高者。")}
        </span>
      </div>
      {live.costs.length > 0 && (
        <div className="paper-table" role="region" aria-label={t("账户费率表")} tabIndex={0}>
          <table aria-label={t("账户费率表")}>
            <thead>
              <tr>
                <th>{t("合约")}</th>
                <th>{t("保证金 每手 / 比例")}</th>
                <th>{t("开仓 每手 / 比例")}</th>
                <th>{t("平今 每手 / 比例")}</th>
                <th>{t("平昨 每手 / 比例")}</th>
                <th>{t("状态")}</th>
                <th>{t("操作")}</th>
              </tr>
            </thead>
            <tbody>
              {live.costs.map(row => {
                const contract = live.contracts.find(item => key(item) === key(row));
                const product = contract ? `${contract.venue}/${contract.product}` : "";
                const c = row.costs;
                const versionKey = `${product}@${day}`;
                return (
                  <tr key={key(row)}>
                    <td>{row.symbol}</td>
                    <td>{c && `${c.margin_per_lot} / ${c.margin_rate}`}</td>
                    <td>{c && `${c.open_fee} / ${c.open_fee_rate}`}</td>
                    <td>{c && `${c.close_today_fee} / ${c.close_today_fee_rate}`}</td>
                    <td>{c && `${c.close_yesterday_fee} / ${c.close_yesterday_fee_rate}`}</td>
                    <td>
                      {t(states[row.state])}
                      {row.error_code ? ` (${row.error_code})` : ""}
                    </td>
                    <td>
                      {c && product && (
                        <button
                          disabled={saved.includes(versionKey)}
                          onClick={() => {
                            try {
                              saveCostTemplate(
                                product,
                                c,
                                t("CTP 账户 {broker}/{user}", {
                                  broker: live.broker.broker_id,
                                  user: live.broker.user_id,
                                }),
                                day,
                              );
                              setSaved([...saved, versionKey]);
                              setError(null);
                            } catch (reason) {
                              setError(asDisplayError(reason));
                            }
                          }}
                        >
                          {saved.includes(versionKey)
                            ? t("已存为 {product} 模板", { product })
                            : t("存为 {product} 费率模板", { product })}
                        </button>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
