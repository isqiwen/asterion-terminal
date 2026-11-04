import { decimalText } from "../../src/bridge/decimal";
import { Side } from "../../src/generated/asterion/common_pb";
import {
  BrokerState,
  CostsState,
  OrderState,
  type Account,
  type Session,
} from "../../src/generated/asterion/trading_pb";
import type { TerminalContext } from "../contract";
import { AccountPolicyEditor } from "./AccountPolicyEditor";
import { ActivityTabs, type Activity } from "./ActivityTabs";
import { key, offsets, statuses, t, type Run } from "./live-shared";

const working = [OrderState.SUBMITTED, OrderState.ACCEPTED, OrderState.PARTIALLY_FILLED];

// The account's positions, orders, fills, rates and risk policy, one tab at a
// time. A working order can be cancelled from its row.
export function AccountRecords({
  record,
  busy,
  run,
  activity,
  setActivity,
  ready,
  stale,
  cancelling,
  cancel,
}: Pick<TerminalContext, "busy"> & {
  record: Account;
  run: Run;
  activity: Activity;
  setActivity: (value: Activity) => void;
  ready: boolean;
  stale: boolean;
  cancelling: readonly string[];
  cancel: (id: string) => Promise<void>;
}) {
  const live = record.session!;
  const policy = live.policy;
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
                <tr key={`${key(p.instrument)}.${p.side}`}>
                  <td>{p.instrument?.symbol}</td>
                  <td>{p.side === Side.BUY ? t("多头") : t("空头")}</td>
                  <td>{decimalText(p.todayLots)}</td>
                  <td>{decimalText(p.yesterdayLots)}</td>
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
                  <tr key={o.brokerKey}>
                    <td>{o.instrument?.symbol}</td>
                    <td>
                      {o.side === Side.BUY ? t("买入") : t("卖出")} / {t(offsets[o.offset])}
                    </td>
                    <td>{decimalText(o.price)}</td>
                    <td>{decimalText(o.lots)}</td>
                    <td>{decimalText(o.filledLots)}</td>
                    <td>
                      {t(statuses[o.state] ?? OrderState[o.state])}
                      {o.ctpError ? ` (${o.ctpError})` : ""}
                    </td>
                    <td>
                      {o.id && working.includes(o.state) && (
                        <button
                          disabled={
                            cancelling.includes(o.id) ||
                            stale ||
                            !(
                              ready ||
                              (live.brokerState === BrokerState.FAILED && live.ctpError === -1008)
                            )
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
                    <td>{f.instrument?.symbol}</td>
                    <td>{f.tradeTime}</td>
                    <td>{decimalText(f.price)}</td>
                    <td>{decimalText(f.lots)}</td>
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
            {t("单笔数量上限")}: {decimalText(policy?.limits?.maxOrderLots)} · {t("总持仓量上限")}:{" "}
            {decimalText(policy?.limits?.maxGrossLots)} · {t("在途委托数上限")}:{" "}
            {policy?.limits?.maxWorkingOrders ?? 0} · {t("价格偏离上限")}:{" "}
            {decimalText(policy?.maxPriceDeviation)}
          </p>
          <p>
            {t("可交易合约")}: {policy?.contracts.map(item => key(item.instrument)).join("、")}
          </p>
        </section>
        <AccountPolicyEditor
          key={live.policyRevision}
          record={record}
          disabled={busy || stale}
          run={run}
        />
      </div>
    </>
  );
}

// The account's margin and commission rates as the broker reports them.
function AccountRates({ live, busy, run }: { live: Session; busy: boolean; run: Run }) {
  const states: Partial<Record<CostsState, string>> = {
    [CostsState.QUERYING]: "查询中",
    [CostsState.READY]: "已返回",
    [CostsState.UNAVAILABLE]: "券商未提供",
  };
  return (
    <section className="account-rates" aria-label={t("账户费率")}>
      <div className="source-actions">
        <button disabled={busy} onClick={() => void run({ case: "queryCosts", value: {} })}>
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
              </tr>
            </thead>
            <tbody>
              {live.costs.map(row => {
                const c = row.costs;
                return (
                  <tr key={key(row.instrument)}>
                    <td>{row.instrument?.symbol}</td>
                    <td>{c && `${decimalText(c.marginPerLot)} / ${decimalText(c.marginRate)}`}</td>
                    <td>{c && `${decimalText(c.openFee)} / ${decimalText(c.openFeeRate)}`}</td>
                    <td>
                      {c && `${decimalText(c.closeTodayFee)} / ${decimalText(c.closeTodayFeeRate)}`}
                    </td>
                    <td>
                      {c &&
                        `${decimalText(c.closeYesterdayFee)} / ${decimalText(c.closeYesterdayFeeRate)}`}
                    </td>
                    <td>
                      {t(states[row.state] ?? CostsState[row.state])}
                      {row.ctpError ? ` (${row.ctpError})` : ""}
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
