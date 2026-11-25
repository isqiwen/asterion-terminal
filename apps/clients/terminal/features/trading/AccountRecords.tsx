import { useEffect, useRef, useState } from "react";
import { decimal, decimalText, groupedText } from "../../src/bridge/decimal";
import { tradingRequest } from "../../src/bridge/trading";
import { Side } from "../../src/generated/asterion/common_pb";
import { getLocale } from "../../src/i18n";
import { ctpErrorSummary } from "../../src/i18n/errors";
import {
  BrokerState,
  CostsState,
  OrderState,
  type Account,
  type ContractCancels,
  type DayClose,
  type Position,
  type Session,
  type Trade,
} from "../../src/generated/asterion/trading_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { useMarket } from "../../src/host/market";
import { useTradingDayFills, useTradingOrders } from "../../src/host/trading-accounts";
import { contractName } from "../../src/ui/contract-names";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { AccountPolicyEditor } from "./AccountPolicyEditor";
import { closableLots, lots } from "./exposure";
import { ActivityTabs, type Activity } from "./ActivityTabs";
import type { Preset } from "./OrderTicket";
import { findPolicyTerms, fundFields, key, offsets, statuses, t, type Run } from "./live-shared";

const pageSize = 50;
const working = [OrderState.SUBMITTED, OrderState.ACCEPTED, OrderState.PARTIALLY_FILLED];
const money = (value: number) =>
  groupedText(decimal((Math.abs(value) < 0.005 ? 0 : value).toFixed(2)));
const orderTone = (state: OrderState): Tone => {
  if (state === OrderState.FILLED) return "ok";
  if (state === OrderState.PARTIALLY_FILLED || state === OrderState.SUBMITTED) return "attention";
  if (state === OrderState.REJECTED) return "danger";
  return "neutral";
};

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
  `${f.side === Side.BUY ? t("买入") : t("卖出")} / ${offsets[f.offset] ? t(offsets[f.offset]!) : "—"}`,
  f.tradeTime,
  decimalText(f.price),
  groupedText(f.lots),
];
const cancelColumns = ["合约", "撤单次数", "拒撤次数"];
const cancelRow = (c: ContractCancels) => [
  c.instrument?.symbol ?? "",
  String(c.cancels),
  String(c.rejectedCancels),
];

const numericColumns = new Set(["今仓", "昨仓", "价格", "手数", "撤单次数", "拒撤次数"]);

function Rows({
  label,
  columns,
  rows,
  tones,
}: {
  label: string;
  columns: string[];
  rows: string[][];
  tones?: readonly (string | undefined)[];
}) {
  return (
    <table aria-label={t(label)}>
      <thead>
        <tr>
          {columns.map(column => (
            <th
              scope="col"
              key={column}
              className={numericColumns.has(column) ? "numeric" : undefined}
            >
              {t(column)}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((row, rowIndex) => (
          <tr key={`${rowIndex}:${row.join(" ")}`}>
            {row.map((cell, index) => (
              <td
                key={index}
                className={
                  numericColumns.has(columns[index]!)
                    ? "numeric"
                    : columns[index] === "方向"
                      ? tones?.[rowIndex]
                      : undefined
                }
              >
                {cell}
              </td>
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
    if (close.funds) {
      lines.push(
        [],
        fundFields.map(([, label]) => t(label)),
      );
      lines.push(fundFields.map(([field]) => groupedText(close.funds?.[field])));
    }
    if (close.positions.length > 0 || close.funds) {
      lines.push(
        [],
        positionColumns.map(column => t(column)),
        ...close.positions.map(positionRow),
      );
    }
    if (close.cancels.length > 0) {
      lines.push(
        [],
        cancelColumns.map(column => t(column)),
        ...close.cancels.map(cancelRow),
      );
    }
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

function HistoryPages({
  offset,
  total,
  loading,
  label,
  change,
}: {
  offset: number;
  total: number;
  loading: boolean;
  label: string;
  change: (value: number) => void;
}) {
  if (!offset && total <= pageSize) return null;
  return (
    <nav className="source-actions" aria-label={t(label)}>
      <button
        type="button"
        aria-disabled={loading || !offset}
        onClick={() => {
          if (!loading && offset) change(offset - pageSize);
        }}
      >
        {t("上一页")}
      </button>
      <button
        type="button"
        aria-disabled={loading || offset + pageSize >= total}
        onClick={() => {
          if (!loading && offset + pageSize < total) change(offset + pageSize);
        }}
      >
        {t("下一页")}
      </button>
    </nav>
  );
}

// The fills of the session's trading day as the counter reports them, or an
// earlier trading day as the account recorded it: its close and its fills.
// `connection` is the Trading connection, not the account the counter names.
function Fills({
  connection,
  live,
  active: open,
}: {
  connection: string;
  live: Session;
  active: boolean;
}) {
  const [day, setDay] = useState("");
  const [offset, setOffset] = useState(0);
  const [exporting, setExporting] = useState(false);
  useEffect(() => {
    setDay("");
    setOffset(0);
  }, [connection]);
  useEffect(() => {
    if (!day) setOffset(0);
  }, [day, live.tradingDay]);
  const { loading, recorded } = useTradingDayFills(
    connection,
    day,
    live.tradingDay,
    offset,
    pageSize,
    open,
  );
  const current =
    recorded.connection === connection &&
    recorded.day === day &&
    recorded.offset === offset &&
    (day || recorded.liveDay === live.tradingDay)
      ? recorded
      : undefined;
  const trades = current?.trades ?? [];
  const fills = trades.map(fillRow);
  const fillTones = trades.map(f => (f.side === Side.BUY ? "ctp-up" : "ctp-down"));
  const close = day ? current?.close : undefined;
  const cancels = current?.close?.cancels ?? [];
  const targetDay = day || live.tradingDay || current?.close?.tradingDay || "";
  const earlier = (recorded.connection === connection ? recorded.days : [])
    .filter(item => item !== live.tradingDay)
    .reverse();
  const pageLots = trades.reduce((sum, item) => sum + lots(item.lots), 0);
  const summaryFunds = !day ? live.funds : undefined;
  const closeProfit = Number(summaryFunds?.closeProfit?.text ?? 0);
  return (
    <div className="paper-table" role="region" aria-label={t("CTP 成交")} tabIndex={0}>
      {(earlier.length > 0 || (targetDay && current) || trades.length > 0) && (
        <div className="source-actions records-toolbar">
          {earlier.length > 0 && (
            <select
              aria-label={t("交易日")}
              value={day}
              onChange={event => {
                setOffset(0);
                setDay(event.target.value);
              }}
            >
              <option value="">{t("当日")}</option>
              {earlier.map(item => (
                <option key={item} value={item}>
                  {item}
                </option>
              ))}
            </select>
          )}
          {targetDay && current && (
            <button
              type="button"
              disabled={exporting}
              onClick={() => {
                setExporting(true);
                void (async () => {
                  try {
                    const exportedTrades: Trade[] = [];
                    let exportedClose: DayClose | undefined;
                    let total: number | undefined;
                    do {
                      const reply = await tradingRequest(connection, {
                        case: "getDay",
                        value: {
                          tradingDay: targetDay,
                          oldestFirst: true,
                          offset: exportedTrades.length,
                          limit:
                            total === undefined
                              ? 200
                              : Math.min(200, total - exportedTrades.length),
                        },
                      });
                      if (reply.result.case !== "getDay") return;
                      const page = reply.result.value;
                      if (total === undefined) {
                        total = page.total;
                        exportedClose = page.close;
                      }
                      if (exportedTrades.length < total && !page.trades.length)
                        throw new Error(t("交易日成交记录已变化，请重新导出。"));
                      exportedTrades.push(...page.trades);
                    } while (exportedTrades.length < total);
                    download(
                      `${connection}-${targetDay}.csv`,
                      dayCsv(targetDay, exportedClose, exportedTrades.reverse().map(fillRow)),
                    );
                  } catch (failure) {
                    notify(failure, { namespace: "asterion.terminal.trading" });
                  } finally {
                    setExporting(false);
                  }
                })();
              }}
            >
              {t("导出 CSV")}
            </button>
          )}
          {trades.length > 0 && (
            <div className="fills-summary">
              <span>{t("共 {n} 笔", { n: String(current?.total || trades.length) })}</span>
              <span>{t("本页 {n} 手", { n: pageLots.toLocaleString(getLocale()) })}</span>
              {summaryFunds && (
                <>
                  <span className={closeProfit > 0 ? "ctp-up" : closeProfit < 0 ? "ctp-down" : ""}>
                    {t("平仓盈亏")} {groupedText(summaryFunds.closeProfit)}
                  </span>
                  <span>
                    {t("手续费")} {groupedText(summaryFunds.commission)}
                  </span>
                </>
              )}
            </div>
          )}
        </div>
      )}
      {close && close.funds && (
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
            tones={close.positions.map(p => (p.side === Side.BUY ? "ctp-up" : "ctp-down"))}
          />
        </>
      )}
      {cancels.length > 0 && (
        <Rows label="撤单统计" columns={cancelColumns} rows={cancels.map(cancelRow)} />
      )}
      <Rows label="CTP 成交" columns={fillColumns} rows={fills} tones={fillTones} />
      {current && !fills.length && <p className="subtle">{t("暂无成交")}</p>}
      <HistoryPages
        offset={offset}
        total={current?.total ?? 0}
        loading={loading}
        label="成交记录分页"
        change={setOffset}
      />
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
  selectedContract,
  onSelectContract,
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
  // Puts a position's close in the order ticket when orders may be placed.
  close?: (preset: Preset) => void;
  // Whether the exchange closes today's and earlier lots separately.
  separate: (exchange: string) => boolean;
  selectedContract?: string;
  onSelectContract?: (contract: string) => void;
}) {
  const live = record.session!;
  const [onlyWorking, setOnlyWorking] = useState(false);
  const [cancelArmed, setCancelArmed] = useState(false);
  const [cancellingAll, setCancellingAll] = useState(false);
  const cancellingAllRef = useRef(false);
  const workingOrders = live.workingOrders.filter(o => o.id);
  const [offset, setOffset] = useState(0);
  useEffect(() => {
    setOffset(0);
  }, [connection, live.tradingDay]);
  const { loading, orders } = useTradingOrders(
    connection,
    live.tradingDay,
    offset,
    pageSize,
    activity === "orders" && !onlyWorking,
  );
  const cancellable =
    !stale && (ready || (live.brokerState === BrokerState.FAILED && live.ctpError === -1008));
  const shownOrders = onlyWorking
    ? workingOrders.slice().reverse()
    : orders?.connection === connection &&
        orders.day === live.tradingDay &&
        orders.offset === offset
      ? orders.rows
      : [];
  const market = useMarket();
  const catalog = market?.board.catalog;
  return (
    <>
      <ActivityTabs
        value={activity}
        onChange={setActivity}
        counts={{
          positions: live.positions.length ? String(live.positions.length) : undefined,
          orders: live.orderCount
            ? workingOrders.length
              ? t("{working} 可撤 / {all}", {
                  working: workingOrders.length,
                  all: live.orderCount,
                })
              : String(live.orderCount)
            : undefined,
          fills: live.tradeCount ? String(live.tradeCount) : undefined,
        }}
      />
      <div hidden={activity !== "positions"}>
        <LivePositions
          live={live}
          close={close}
          separate={separate}
          selectedContract={selectedContract}
          onSelectContract={onSelectContract}
        />
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
          {workingOrders.length > 0 && (
            <button
              type="button"
              className={cancelArmed ? "danger" : "danger-quiet"}
              disabled={!cancellable || cancellingAll}
              onBlur={() => setCancelArmed(false)}
              onClick={() => {
                if (!cancelArmed) {
                  setCancelArmed(true);
                  return;
                }
                if (cancellingAllRef.current) return;
                setCancelArmed(false);
                cancellingAllRef.current = true;
                setCancellingAll(true);
                const targets = [...workingOrders];
                // One cancel each, in turn; none is sent again on failure.
                void (async () => {
                  try {
                    for (const order of targets) await cancel(order.id);
                  } finally {
                    cancellingAllRef.current = false;
                    setCancellingAll(false);
                  }
                })();
              }}
            >
              {cancelArmed
                ? t("确认撤掉 {count} 笔", { count: workingOrders.length })
                : t("全部撤单")}
            </button>
          )}
        </div>
        <div className="paper-table" role="region" aria-label={t("CTP 委托记录")} tabIndex={0}>
          <table aria-label={t("CTP 委托记录")}>
            <thead>
              <tr>
                <th scope="col">{t("合约")}</th>
                <th scope="col">{t("方向 / 开平")}</th>
                <th scope="col" className="numeric">
                  {t("限价")}
                </th>
                <th scope="col" className="numeric">
                  {t("手数")}
                </th>
                <th scope="col" className="numeric">
                  {t("已成交")}
                </th>
                <th scope="col">{t("状态")}</th>
                <th scope="col">{t("操作")}</th>
              </tr>
            </thead>
            <tbody>
              {shownOrders.map(o => {
                const contractId = key(o.instrument);
                const name = o.instrument ? contractName(o.instrument, catalog) : undefined;
                const total = lots(o.lots);
                const filled = lots(o.filledLots);
                const partialShare =
                  total > 0 && filled > 0 && filled < total ? (filled / total) * 100 : 0;
                const errSummary = o.ctpError ? ctpErrorSummary(o.ctpError) : "";
                return (
                  <tr
                    key={o.brokerKey}
                    data-selected={
                      (selectedContract && selectedContract === contractId) || undefined
                    }
                    onClick={() => {
                      if (o.instrument && onSelectContract) onSelectContract(contractId);
                    }}
                  >
                    <td>
                      <span className="record-contract">
                        <span>{o.instrument?.symbol}</span>
                        {name && <small>{name}</small>}
                      </span>
                    </td>
                    <td className={o.side === Side.BUY ? "ctp-up" : "ctp-down"}>
                      {o.side === Side.BUY ? t("买入") : t("卖出")} /{" "}
                      {offsets[o.offset] ? t(offsets[o.offset]!) : "—"}
                    </td>
                    <td className="numeric">{decimalText(o.price)}</td>
                    <td className="numeric">{groupedText(o.lots)}</td>
                    <td className="numeric">
                      <span>{groupedText(o.filledLots)}</span>
                      {partialShare > 0 && (
                        <span className="order-fill-bar" aria-hidden="true">
                          <span style={{ width: `${partialShare}%` }} />
                        </span>
                      )}
                    </td>
                    <td>
                      <span className="order-status-cell" title={errSummary || undefined}>
                        <StatusBadge tone={orderTone(o.state)}>
                          {statuses[o.state] ? t(statuses[o.state]!) : "—"}
                        </StatusBadge>
                        {o.ctpError ? (
                          <small className="order-error-code">({o.ctpError})</small>
                        ) : null}
                      </span>
                    </td>
                    <td>
                      {o.id && working.includes(o.state) && (
                        <button
                          type="button"
                          className="danger-quiet"
                          disabled={cancelling.includes(o.id) || !cancellable}
                          onClick={event => {
                            event.stopPropagation();
                            void cancel(o.id);
                          }}
                        >
                          {t("撤单")}
                        </button>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          {!live.orderCount && <p className="subtle">{t("暂无委托")}</p>}
          {!onlyWorking && (
            <HistoryPages
              offset={offset}
              total={live.orderCount}
              loading={loading}
              label="委托记录分页"
              change={setOffset}
            />
          )}
        </div>
      </div>
      <div hidden={activity !== "fills"}>
        <Fills connection={connection} live={live} active={activity === "fills"} />
      </div>
      {activity === "rates" && (
        <div>
          {ready ? (
            <AccountRates live={live} busy={busy} run={run} />
          ) : (
            <p className="subtle">{t("未连接柜台")}</p>
          )}
        </div>
      )}
      {activity === "policy" && (
        <div>
          <AccountPolicyEditor
            key={live.policyRevision}
            connection={connection}
            record={record}
            disabled={busy || stale}
            run={run}
          />
        </div>
      )}
    </>
  );
}

// Live positions with what the broker reports of them, each with its close.
// Closable lots leave out those that working close orders already take.
function LivePositions({
  live,
  close,
  separate,
  selectedContract,
  onSelectContract,
}: {
  live: Session;
  close?: (preset: Preset) => void;
  separate: (exchange: string) => boolean;
  selectedContract?: string;
  onSelectContract?: (contract: string) => void;
}) {
  const market = useMarket();
  const catalog = market?.board.catalog;
  const quotes = market?.board.rows;
  if (!live.positions.length)
    return (
      <div className="paper-table" role="region" aria-label={t("CTP 持仓")}>
        <p className="subtle">{t("暂无持仓")}</p>
      </div>
    );
  let sumTotal = 0;
  let sumToday = 0;
  let sumYesterday = 0;
  let sumClosable = 0;
  let sumProfit = 0;
  let sumMargin = 0;

  const renderedRows = live.positions.map(p => {
    const contractId = key(p.instrument);
    const today = lots(p.todayLots);
    const yesterday = lots(p.yesterdayLots);
    const total = today + yesterday;
    // Where the exchange keeps today's and earlier lots apart, yesterday's close first.
    const apart = separate(p.instrument?.exchange ?? "");
    const {
      closing,
      today: closableToday,
      yesterday: closableYesterday,
      total: closable,
    } = closableLots(live, p, apart);
    sumTotal += total;
    sumToday += today;
    sumYesterday += yesterday;
    sumClosable += closable;
    const term = findPolicyTerms(live.policy?.contracts, p.instrument);
    const listed = !term
      ? catalog?.contracts.find(
          item =>
            item.instrument?.exchange === p.instrument?.exchange &&
            item.instrument?.symbol.toUpperCase() === p.instrument?.symbol.toUpperCase(),
        )
      : undefined;
    const multiplier = Number(term?.multiplier?.text ?? listed?.multiplier ?? 0);
    const priceTick = term?.priceTick?.text ?? listed?.priceTick?.text ?? "";
    const decimals = Math.max(2, (priceTick.split(".")[1] ?? "").length);
    const cost = Number(p.cost?.text ?? 0);
    const average = multiplier && total && cost ? cost / (total * multiplier) : undefined;
    const profit = Number(p.profit?.text ?? 0);
    const margin = Number(p.margin?.text ?? 0);
    sumProfit += profit;
    sumMargin += margin;
    const roi = margin > 0 ? (profit / margin) * 100 : undefined;
    const lastPrice = quotes?.find(row => key(row.instrument) === contractId)?.quote?.last?.text;
    const name = p.instrument ? contractName(p.instrument, catalog) : undefined;
    const preset = (): Preset => ({
      contract: contractId,
      offset: !apart ? "close" : closableYesterday > 0 ? "close_yesterday" : "close_today",
      quantity: String(
        !apart ? closable : closableYesterday > 0 ? closableYesterday : closableToday,
      ),
      side: closing,
    });
    return (
      <tr
        key={`${contractId} ${p.side}`}
        data-selected={(selectedContract && selectedContract === contractId) || undefined}
        onClick={() => {
          if (p.instrument && onSelectContract) onSelectContract(contractId);
        }}
        onDoubleClick={() => {
          if (closable && close) close(preset());
        }}
      >
        <td>
          <span className="record-contract">
            <span>{p.instrument?.symbol}</span>
            {name && <small>{name}</small>}
          </span>
        </td>
        <td className={p.side === Side.BUY ? "ctp-up" : "ctp-down"}>
          {p.side === Side.BUY ? t("多头") : t("空头")}
        </td>
        <td className="numeric">{total.toLocaleString(getLocale())}</td>
        <td className="numeric">{groupedText(p.todayLots)}</td>
        <td className="numeric">{groupedText(p.yesterdayLots)}</td>
        <td className="numeric">{closable.toLocaleString(getLocale())}</td>
        <td className="numeric">{average === undefined ? "—" : average.toFixed(decimals)}</td>
        <td className="numeric">{lastPrice ?? "—"}</td>
        <td className={`numeric ${profit > 0 ? "ctp-up" : profit < 0 ? "ctp-down" : ""}`}>
          <span className="position-pnl">
            <span>{groupedText(p.profit)}</span>
            {roi !== undefined && (
              <small title={t("按占用保证金计算收益率")}>
                {roi >= 0 ? "+" : ""}
                {roi.toFixed(1)}%
              </small>
            )}
          </span>
        </td>
        <td className="numeric">{groupedText(p.margin)}</td>
        <td>
          <button
            type="button"
            disabled={!closable || !close}
            onClick={event => {
              event.stopPropagation();
              close?.(preset());
            }}
          >
            {t("平仓")}
          </button>
        </td>
      </tr>
    );
  });

  const sumRoi = sumMargin > 0 ? (sumProfit / sumMargin) * 100 : undefined;
  return (
    <div className="paper-table" role="region" aria-label={t("CTP 持仓")} tabIndex={0}>
      <table aria-label={t("CTP 持仓")}>
        <thead>
          <tr>
            {[
              "合约",
              "方向",
              "总仓",
              "今仓",
              "昨仓",
              "可平",
              "均价",
              "最新价",
              "持仓盈亏",
              "保证金",
            ].map(column => (
              <th
                scope="col"
                key={column}
                className={column === "合约" || column === "方向" ? undefined : "numeric"}
              >
                {t(column)}
              </th>
            ))}
            <th scope="col">
              <span className="sr-only">{t("操作")}</span>
            </th>
          </tr>
        </thead>
        <tbody>{renderedRows}</tbody>
        <tfoot>
          <tr className="records-summary-row">
            <td>{t("合计 ({n})", { n: String(live.positions.length) })}</td>
            <td />
            <td className="numeric">{sumTotal.toLocaleString(getLocale())}</td>
            <td className="numeric">{sumToday.toLocaleString(getLocale())}</td>
            <td className="numeric">{sumYesterday.toLocaleString(getLocale())}</td>
            <td className="numeric">{sumClosable.toLocaleString(getLocale())}</td>
            <td className="numeric">—</td>
            <td className="numeric">—</td>
            <td className={`numeric ${sumProfit > 0 ? "ctp-up" : sumProfit < 0 ? "ctp-down" : ""}`}>
              <span className="position-pnl">
                <span>{money(sumProfit)}</span>
                {sumRoi !== undefined && (
                  <small>
                    {sumRoi >= 0 ? "+" : ""}
                    {sumRoi.toFixed(1)}%
                  </small>
                )}
              </span>
            </td>
            <td className="numeric">{money(sumMargin)}</td>
            <td />
          </tr>
        </tfoot>
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
        <button
          type="button"
          disabled={busy}
          title={t("多空保证金不同时取较高者")}
          onClick={() => void run({ case: "queryCosts", value: {} })}
        >
          {t("查询账户费率")}
        </button>
      </div>
      {live.costs.length > 0 && (
        <div className="paper-table" role="region" aria-label={t("账户费率表")} tabIndex={0}>
          <table aria-label={t("账户费率表")}>
            <thead>
              <tr>
                <th scope="col">{t("合约")}</th>
                <th scope="col" className="numeric">
                  {t("保证金 每手 / 比例")}
                </th>
                <th scope="col" className="numeric">
                  {t("开仓 每手 / 比例")}
                </th>
                <th scope="col" className="numeric">
                  {t("平今 每手 / 比例")}
                </th>
                <th scope="col" className="numeric">
                  {t("平昨 每手 / 比例")}
                </th>
                <th scope="col">{t("状态")}</th>
              </tr>
            </thead>
            <tbody>
              {live.costs.map(row => {
                const c = row.costs;
                return (
                  <tr key={key(row.instrument)}>
                    <td>{row.instrument?.symbol}</td>
                    <td className="numeric">
                      {c && `${decimalText(c.marginPerLot)} / ${decimalText(c.marginRate)}`}
                    </td>
                    <td className="numeric">
                      {c && `${decimalText(c.openFee)} / ${decimalText(c.openFeeRate)}`}
                    </td>
                    <td className="numeric">
                      {c && `${decimalText(c.closeTodayFee)} / ${decimalText(c.closeTodayFeeRate)}`}
                    </td>
                    <td className="numeric">
                      {c &&
                        `${decimalText(c.closeYesterdayFee)} / ${decimalText(c.closeYesterdayFeeRate)}`}
                    </td>
                    <td>
                      {states[row.state] ? t(states[row.state]!) : "—"}
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
