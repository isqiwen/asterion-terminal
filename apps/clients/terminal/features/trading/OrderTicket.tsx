import { useState } from "react";
import { useMarket } from "../../src/host/market";
import { decimal, decimalText } from "../../src/bridge/decimal";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import type { Account } from "../../src/generated/asterion/trading_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { explicitCloseBuckets } from "../../src/ui/closePolicy";
import { command, key, t, type Run } from "./live-shared";

const offsetValues = {
  open: Offset.OPEN,
  close_today: Offset.CLOSE_TODAY,
  close_yesterday: Offset.CLOSE_YESTERDAY,
  close: Offset.CLOSE,
};

// The owner's limit order on one allowed contract. Buying and selling are
// separate buttons that name the account, so pressing Enter in a field never
// sends an order.
export function OrderTicket({
  record,
  snapshot,
  busy,
  accountName,
  run,
  onSent,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  record: Account;
  accountName: string;
  run: Run;
  onSent: () => void;
}) {
  const [order, setOrder] = useState<{
    contract: string;
    offset: keyof typeof offsetValues;
    quantity: string;
    price: string;
  }>({
    contract: "",
    offset: "open",
    quantity: "1",
    price: "",
  });
  const contracts = record.session?.policy?.contracts ?? [];
  const traded = contracts.find(item => key(item.instrument) === order.contract) ?? contracts[0];
  const exchange = traded?.instrument?.exchange ?? "";
  const symbol = traded?.instrument?.symbol ?? "";
  const explicitBuckets = explicitCloseBuckets(snapshot, exchange);
  const offset =
    order.offset === "open"
      ? "open"
      : explicitBuckets
        ? order.offset === "close"
          ? "close_today"
          : order.offset
        : "close";
  const quote = useMarket()?.board.rows.find(
    row => row.instrument?.exchange === exchange && row.instrument?.symbol === symbol,
  )?.quote;
  const submit = (side: Side, form: HTMLFormElement | null) => {
    if (!traded || !form?.reportValidity()) return;
    void command(run, record, () => ({
      case: "submitOrder",
      value: {
        orderId: crypto.randomUUID(),
        instrument: traded.instrument,
        side,
        offset: offsetValues[offset],
        lots: decimal(order.quantity),
        price: decimal(order.price),
      },
    })).then(sent => sent && onSent());
  };
  return (
    <form aria-label={t("CTP 委托")} onSubmit={event => event.preventDefault()}>
      <fieldset disabled={busy}>
        <div className="futures-fields">
          <label>
            {t("合约")}
            <select
              aria-label={t("委托合约")}
              value={traded ? key(traded.instrument) : ""}
              onChange={event => setOrder({ ...order, contract: event.target.value })}
            >
              {contracts.map(item => (
                <option key={key(item.instrument)} value={key(item.instrument)}>
                  {item.instrument?.exchange} · {item.instrument?.symbol}
                </option>
              ))}
            </select>
          </label>
          <label>
            {t("开平仓")}
            <select
              aria-label={t("开平仓")}
              value={offset}
              onChange={event =>
                setOrder({ ...order, offset: event.target.value as typeof order.offset })
              }
            >
              <option value="open">{t("开仓")}</option>
              {explicitBuckets ? (
                <>
                  <option value="close_today">{t("平今")}</option>
                  <option value="close_yesterday">{t("平昨")}</option>
                </>
              ) : (
                <option value="close">{t("平仓")}</option>
              )}
            </select>
          </label>
          <label>
            {t("委托手数")}
            <input
              aria-label={t("委托手数")}
              inputMode="numeric"
              value={order.quantity}
              onChange={event => setOrder({ ...order, quantity: event.target.value })}
              required
            />
          </label>
          <label>
            {t("限价")}
            <input
              aria-label={t("限价")}
              inputMode="decimal"
              value={order.price}
              placeholder={
                traded ? t("最小变动 {tick}", { tick: decimalText(traded.priceTick) }) : ""
              }
              onChange={event => setOrder({ ...order, price: event.target.value })}
              required
            />
          </label>
        </div>
        {quote && (
          <div className="order-quotes" role="group" aria-label={t("按行情填入限价")}>
            {(
              [
                ["last", "最新 {price}", quote.last?.text],
                ["bid", "买一 {price}", quote.bids[0]?.price?.text],
                ["ask", "卖一 {price}", quote.asks[0]?.price?.text],
              ] as const
            ).map(
              ([field, label, price]) =>
                price && (
                  <button key={field} type="button" onClick={() => setOrder({ ...order, price })}>
                    {t(label, { price })}
                  </button>
                ),
            )}
          </div>
        )}
        <div className="source-actions">
          {[Side.BUY, Side.SELL].map(side => (
            <button
              key={side}
              type="button"
              className="primary"
              onClick={event => submit(side, event.currentTarget.form)}
            >
              {t(side === Side.BUY ? "买入" : "卖出")} <small>{accountName}</small>
            </button>
          ))}
          <span className="subtle">{t("委托先写入本机交易记录再发送；断线后不会自动重发。")}</span>
        </div>
      </fieldset>
    </form>
  );
}
