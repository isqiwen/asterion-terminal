import { useState } from "react";
import type { LiveAction } from "../../src/bridge/commands";
import type { LiveSession } from "../../src/bridge/client";
import { explicitCloseBuckets, type TerminalContext } from "../contract";
import { key, t } from "./live-shared";

// The owner's limit order on one allowed contract. Buying and selling are
// separate buttons that name the account, so pressing Enter in a field never
// sends an order.
export function OrderTicket({
  live,
  snapshot,
  busy,
  accountName,
  act,
  onSent,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  live: LiveSession;
  accountName: string;
  act: (params: LiveAction) => Promise<boolean>;
  onSent: () => void;
}) {
  const [order, setOrder] = useState<{
    contract: string;
    offset: Extract<LiveAction, { action: "submit" }>["offset"];
    quantity: string;
    price: string;
  }>({
    contract: "",
    offset: "open",
    quantity: "1",
    price: "",
  });
  const traded = live.contracts.find(item => key(item) === order.contract) ?? live.contracts[0];
  const explicitBuckets = explicitCloseBuckets(snapshot, traded?.venue ?? "");
  const offset =
    order.offset === "open"
      ? "open"
      : explicitBuckets
        ? order.offset === "close"
          ? "close_today"
          : order.offset
        : "close";
  const quote = snapshot?.market?.subscriptions.find(
    row => row.venue === traded?.venue && row.symbol === traded?.symbol,
  )?.quote;
  const submit = (side: "buy" | "sell", form: HTMLFormElement | null) => {
    if (!traded || !form?.reportValidity()) return;
    void act({
      action: "submit",
      order_id: crypto.randomUUID(),
      venue: traded.venue,
      symbol: traded.symbol,
      side,
      offset,
      quantity: order.quantity,
      price: order.price,
    }).then(sent => sent && onSent());
  };
  return (
    <form aria-label={t("CTP 委托")} onSubmit={event => event.preventDefault()}>
      <fieldset disabled={busy}>
        <div className="futures-fields">
          <label>
            {t("合约")}
            <select
              aria-label={t("委托合约")}
              value={traded ? key(traded) : ""}
              onChange={event => setOrder({ ...order, contract: event.target.value })}
            >
              {live.contracts.map(item => (
                <option key={key(item)} value={key(item)}>
                  {item.venue} · {item.symbol}
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
              placeholder={traded ? t("最小变动 {tick}", { tick: traded.price_increment }) : ""}
              onChange={event => setOrder({ ...order, price: event.target.value })}
              required
            />
          </label>
        </div>
        {quote && (
          <div className="order-quotes" role="group" aria-label={t("按行情填入限价")}>
            {(
              [
                ["last", "最新 {price}"],
                ["bid", "买一 {price}"],
                ["ask", "卖一 {price}"],
              ] as const
            ).map(
              ([field, label]) =>
                quote[field] && (
                  <button
                    key={field}
                    type="button"
                    onClick={() => setOrder({ ...order, price: quote[field]! })}
                  >
                    {t(label, { price: quote[field]! })}
                  </button>
                ),
            )}
          </div>
        )}
        <div className="source-actions">
          {(["buy", "sell"] as const).map(side => (
            <button
              key={side}
              type="button"
              className="primary"
              onClick={event => submit(side, event.currentTarget.form)}
            >
              {t(side === "buy" ? "买入" : "卖出")} <small>{accountName}</small>
            </button>
          ))}
          <span className="subtle">{t("委托先写入本机交易记录再发送；断线后不会自动重发。")}</span>
        </div>
      </fieldset>
    </form>
  );
}
