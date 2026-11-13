import { listSeparator } from "../../src/i18n";
import { useEffect, useState } from "react";
import { decimalText, groupedText } from "../../src/bridge/decimal";
import { tradingRequest } from "../../src/bridge/trading";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import {
  BrokerState,
  CostsState,
  OrderState,
  type Account,
  type DayClose,
  type Position,
  type Session,
  type Trade,
} from "../../src/generated/asterion/trading_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { AccountPolicyEditor } from "./AccountPolicyEditor";
import { ActivityTabs, type Activity } from "./ActivityTabs";
import type { Preset } from "./OrderTicket";
import { fundFields, key, offsets, statuses, t, type Run } from "./live-shared";

const working = [OrderState.SUBMITTED, OrderState.ACCEPTED, OrderState.PARTIALLY_FILLED];

// Rows of positions and fills, the same on the page and in an exported day.
const positionColumns = ["合约", "方向", "今仓", "昨仓"];
const positionRow = (p: Position) => [
  p.instrument?.symbol ?? "",
  p.side === Side.BUY ? t("多头") : t("空头"),
  groupedText(p.todayLots),
  groupedText(p.yesterdayLots),
];
const fillColumns = ["合约", "方向", "时间", "价格", "手数"];
const fillRow = (f: Trade) => [
  f.instrument?.symbol ?? "",
  `${f.side === Side.BUY ? t("买入") : t("卖出")} / ${t(offsets[f.offset])}`,
  f.tradeTime,
  decimalText(f.price),
  groupedText(f.lots),
];

function Rows({ label, columns, rows }: { label: string; columns: string[]; rows: string[][] }) {
  return (
    <table aria-label={t(label)}>
      <thead>
        <tr>
          {columns.map(column => (
            <th key={column}>{t(column)}</th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map(row => (
          <tr key={row.join(" ")}>
            {row.map((cell, index) => (
              <td key={index}>{cell}</td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

// A recorded day as CSV: its close, when recorded, then its fills.
export function dayCsv(tradingDay: string, close: DayClose | undefined, fills: string[][]) {
  const lines: string[][] = [[t("交易日"), tradingDay]];
  if (close) {
    lines.push(
      [],
      fundFields.map(([, label]) => t(label)),
    );
    lines.push(fundFields.map(([field]) => groupedText(close.funds?.[field])));
    lines.push(
      [],
      positionColumns.map(column => t(column)),
      ...close.positions.map(positionRow),
    );
  }
  lines.push(
    [],
    fillColumns.map(column => t(column)),
    ...fills,
  );
  const cell = (value: string) =>
    /[",\n]/.test(value) ? `"${value.replaceAll('"', '""')}"` : value;
  return lines.map(line => line.map(cell).join(",")).join("\n") + "\n";
}

function download(name: string, text: string) {
  // The byte order mark lets spreadsheet programs read the Chinese headers as UTF-8.
  const url = URL.createObjectURL(new Blob(["\uFEFF" + text], { type: "text/csv" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = name;
  link.click();
  URL.revokeObjectURL(url);
}

// The fills of the session's trading day as the counter reports them, or an
// earlier trading day as the account recorded it: its close and its fills.
// `connection` is the Trading connection, not the account the counter names.
function Fills({ connection, live }: { connection: string; live: Session }) {
  const [day, setDay] = useState("");
  const [recorded, setRecorded] = useState<{ days: string[]; trades: Trade[]; close?: DayClose }>({
    days: [],
    trades: [],
  });
  // A new day's first fill adds that day to the choices.
  const today = live.trades.length > 0;
  useEffect(() => {
    let active = true;
    void tradingRequest(connection, { case: "getDay", value: { tradingDay: day } })
      .then(reply => {
        if (active && reply.result.case === "getDay")
          setRecorded({
            days: reply.result.value.tradingDays,
            trades: reply.result.value.trades,
            close: reply.result.value.close,
          });
      })
      .catch(failure => active && notify(failure, { namespace: "asterion.terminal.trading" }));
    return () => {
      active = false;
    };
  }, [connection, day, today]);
  const fills = (day ? recorded.trades : live.trades).slice().reverse().map(fillRow);
  const close = day ? recorded.close : undefined;
  const earlier = recorded.days.filter(item => item !== live.tradingDay).reverse();
  return (
    <div className="paper-table" role="region" aria-label={t("CTP 成交")} tabIndex={0}>
      {earlier.length > 0 && (
        <select aria-label={t("交易日")} value={day} onChange={event => setDay(event.target.value)}>
          <option value="">{t("当日")}</option>
          {earlier.map(item => (
            <option key={item} value={item}>
              {item}
            </option>
          ))}
        </select>
      )}
      {day && (
        <button onClick={() => download(`${connection}-${day}.csv`, dayCsv(day, close, fills))}>
          {t("导出 CSV")}
        </button>
      )}
      {close && (
        <>
          <dl className="paper-metrics">
            {fundFields.map(([field, label]) => (
              <div key={field}>
                <dt>{t(label)}</dt>
                <dd>{groupedText(close.funds?.[field])}</dd>
              </div>
            ))}
          </dl>
          <Rows
            label="收盘持仓"
            columns={positionColumns}
            rows={close.positions.map(positionRow)}
          />
        </>
      )}
      <Rows label="CTP 成交" columns={fillColumns} rows={fills} />
      {!fills.length && <p className="subtle">{t("暂无成交")}</p>}
    </div>
  );
}

// The account's positions, orders, fills, rates and risk policy, one tab at a
// time. A working order can be cancelled from its row.
export function AccountRecords({
  connection,
  record,
  busy,
  run,
  activity,
  setActivity,
  ready,
  stale,
  cancelling,
  cancel,
  close,
  separate,
}: Pick<TerminalContext, "busy"> & {
  connection: string;
  record: Account;
  run: Run;
  activity: Activity;
  setActivity: (value: Activity) => void;
  ready: boolean;
  stale: boolean;
  cancelling: readonly string[];
  cancel: (id: string) => Promise<void>;
  // Puts a position's close in the order ticket.
  close: (preset: Preset) => void;
  // Whether the exchange closes today's and earlier lots separately.
  separate: (exchange: string) => boolean;
}) {
  const live = record.session!;
  const policy = live.policy;
  const [onlyWorking, setOnlyWorking] = useState(false);
  const [cancelArmed, setCancelArmed] = useState(false);
  const workingOrders = live.orders.filter(o => o.id && working.includes(o.state));
  const cancellable =
    !stale && (ready || (live.brokerState === BrokerState.FAILED && live.ctpError === -1008));
  const shownOrders = (onlyWorking ? workingOrders : live.orders).slice().reverse();
  return (
    <>
      <ActivityTabs
        value={activity}
        onChange={setActivity}
        counts={{
          positions: live.positions.length ? String(live.positions.length) : undefined,
          orders: live.orders.length
            ? workingOrders.length
              ? t("{working} 可撤 / {all}", {
                  working: workingOrders.length,
                  all: live.orders.length,
                })
              : String(live.orders.length)
            : undefined,
          fills: live.trades.length ? String(live.trades.length) : undefined,
        }}
      />
      <div hidden={activity !== "positions"}>
        <LivePositions live={live} close={close} separate={separate} />
      </div>
      <div hidden={activity !== "orders"}>
        <div className="source-actions">
          <label className="checkbox">
            <input
              type="checkbox"
              checked={onlyWorking}
              onChange={event => setOnlyWorking(event.target.checked)}
            />
            {t("只看可撤")}
          </label>
          {workingOrders.length > 0 &&
            (cancelArmed ? (
              <button
                className="danger"
                disabled={!cancellable}
                onBlur={() => setCancelArmed(false)}
                onClick={() => {
                  setCancelArmed(false);
                  // One cancel each, in turn; none is sent again on failure.
                  void (async () => {
                    for (const order of workingOrders) await cancel(order.id);
                  })();
                }}
              >
                {t("确认撤掉 {count} 笔", { count: workingOrders.length })}
              </button>
            ) : (
              <button disabled={!cancellable} onClick={() => setCancelArmed(true)}>
                {t("全部撤单")}
              </button>
            ))}
        </div>
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
              {shownOrders.map(o => (
                <tr key={o.brokerKey}>
                  <td>{o.instrument?.symbol}</td>
                  <td>
                    {o.side === Side.BUY ? t("买入") : t("卖出")} / {t(offsets[o.offset])}
                  </td>
                  <td>{decimalText(o.price)}</td>
                  <td>{groupedText(o.lots)}</td>
                  <td>{groupedText(o.filledLots)}</td>
                  <td>
                    {t(statuses[o.state] ?? OrderState[o.state])}
                    {o.ctpError ? ` (${o.ctpError})` : ""}
                  </td>
                  <td>
                    {o.id && working.includes(o.state) && (
                      <button
                        disabled={cancelling.includes(o.id) || !cancellable}
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
        <Fills connection={connection} live={live} />
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
            {t("可交易合约")}:{" "}
            {policy?.contracts.map(item => key(item.instrument)).join(listSeparator())}
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

// Live positions with what the broker reports of them, each with its close.
// Closable lots leave out those that working close orders already take.
function LivePositions({
  live,
  close,
  separate,
}: {
  live: Session;
  close: (preset: Preset) => void;
  separate: (exchange: string) => boolean;
}) {
  const terms = new Map((live.policy?.contracts ?? []).map(item => [key(item.instrument), item]));
  if (!live.positions.length)
    return (
      <div className="paper-table" role="region" aria-label={t("CTP 持仓")} tabIndex={0}>
        <p className="subtle">{t("暂无持仓")}</p>
      </div>
    );
  return (
    <div className="paper-table" role="region" aria-label={t("CTP 持仓")} tabIndex={0}>
      <table aria-label={t("CTP 持仓")}>
        <thead>
          <tr>
            {["合约", "方向", "总仓", "今仓", "昨仓", "可平", "均价", "持仓盈亏", "保证金"].map(
              column => (
                <th
                  key={column}
                  className={column === "合约" || column === "方向" ? undefined : "numeric"}
                >
                  {t(column)}
                </th>
              ),
            )}
            <th />
          </tr>
        </thead>
        <tbody>
          {live.positions.map(p => {
            const today = Number(p.todayLots?.text ?? 0);
            const yesterday = Number(p.yesterdayLots?.text ?? 0);
            const total = today + yesterday;
            // Closing a long sells; closing a short buys.
            const closing = p.side === Side.BUY ? Side.SELL : Side.BUY;
            const held = live.orders
              .filter(
                o =>
                  o.id &&
                  working.includes(o.state) &&
                  key(o.instrument) === key(p.instrument) &&
                  o.side === closing &&
                  o.offset !== Offset.OPEN,
              )
              .reduce(
                (sum, o) => sum + Number(o.lots?.text ?? 0) - Number(o.filledLots?.text ?? 0),
                0,
              );
            const closable = Math.max(0, total - held);
            const multiplier = Number(terms.get(key(p.instrument))?.multiplier?.text ?? 0);
            const cost = Number(p.cost?.text ?? 0);
            const average = multiplier && total && cost ? cost / (total * multiplier) : undefined;
            const profit = Number(p.profit?.text ?? 0);
            // Where the exchange keeps today's lots apart they close first, on their own.
            const apart = separate(p.instrument?.exchange ?? "");
            const preset = (): Preset => ({
              contract: key(p.instrument),
              offset: !apart ? "close" : today ? "close_today" : "close_yesterday",
              quantity: String(apart ? Math.min(closable, today || yesterday) : closable),
              side: closing,
            });
            return (
              <tr key={`${key(p.instrument)} ${p.side}`} onDoubleClick={() => close(preset())}>
                <td>{p.instrument?.symbol}</td>
                <td>{p.side === Side.BUY ? t("多头") : t("空头")}</td>
                <td className="numeric">{total}</td>
                <td className="numeric">{groupedText(p.todayLots)}</td>
                <td className="numeric">{groupedText(p.yesterdayLots)}</td>
                <td className="numeric">{closable}</td>
                <td className="numeric">{average === undefined ? "—" : average.toFixed(2)}</td>
                <td className={`numeric ${profit > 0 ? "ctp-up" : profit < 0 ? "ctp-down" : ""}`}>
                  {groupedText(p.profit)}
                </td>
                <td className="numeric">{groupedText(p.margin)}</td>
                <td>
                  <button disabled={!closable} onClick={() => close(preset())}>
                    {t("平仓")}
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
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
