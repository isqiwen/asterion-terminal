import { useEffect, useRef, useState } from "react";
import { useMarket } from "../../src/host/market";
import { decimal, decimalText, priceTone } from "../../src/bridge/decimal";
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

// What a position's close puts in the ticket; it is never sent by itself.
export type Preset = {
  contract: string;
  offset: "open" | "close_today" | "close_yesterday" | "close";
  quantity: string;
  side: Side;
};

// The owner's limit order on one allowed contract. Buying and selling are
// separate buttons that name the account, so pressing Enter in a field never
// sends an order. With `confirm` (a real-money account) either button first
// shows what will be sent. `preset` fills the fields and marks the side to press.
export function OrderTicket({
  record,
  snapshot,
  busy,
  accountName,
  confirm,
  run,
  onSent,
  preset,
  initialContract,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  record: Account;
  accountName: string;
  confirm: boolean;
  run: Run;
  onSent: () => void;
  preset?: Preset;
  initialContract?: string;
}) {
  const [pending, setPending] = useState<Side>();
  const [order, setOrder] = useState<{
    contract: string;
    offset: keyof typeof offsetValues;
    quantity: string;
    price: string;
  }>({
    contract: initialContract ?? "",
    offset: "open",
    quantity: "1",
    price: "",
  });
  useEffect(() => {
    if (initialContract) setOrder(current => ({ ...current, contract: initialContract }));
  }, [initialContract]);
  const [suggested, setSuggested] = useState<Side>();
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
  const rows = useMarket()?.board.rows;
  const quoteOf = (contract: string) => rows?.find(row => key(row.instrument) === contract)?.quote;
  const quote = quoteOf(traded ? key(traded.instrument) : "");
  // A preset applies once; selling takes the best bid and buying the best ask, if quoted.
  const applied = useRef<Preset>(undefined);
  useEffect(() => {
    if (!preset || applied.current === preset) return;
    applied.current = preset;
    const book = rows?.find(row => key(row.instrument) === preset.contract)?.quote;
    const price = (preset.side === Side.SELL ? book?.bids[0] : book?.asks[0])?.price?.text;
    setPending(undefined);
    setSuggested(preset.side);
    setOrder(current => ({ ...current, ...preset, price: price ?? current.price }));
  }, [preset, rows]);
  const offsetLabels = {
    open: "开仓",
    close_today: "平今",
    close_yesterday: "平昨",
    close: "平仓",
  };
  const fill = (price: string | undefined) => price && setOrder(current => ({ ...current, price }));
  const submit = (side: Side, form: HTMLFormElement | null) => {
    if (!traded || !form?.reportValidity()) return;
    if (confirm && pending !== side) return setPending(side);
    setPending(undefined);
    setSuggested(undefined);
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
        <div className="order-book" role="group" aria-label={t("五档行情")}>
          {quote ? (
            <>
              <button type="button" className="order-last" onClick={() => fill(quote.last?.text)}>
                {t("最新 {price}", { price: quote.last?.text ?? "—" })}
              </button>
              {[4, 3, 2, 1, 0].map(level => {
                const ask = quote.asks[level];
                return (
                  <button
                    key={`ask${level}`}
                    type="button"
                    className="order-ask"
                    data-tone={priceTone(ask?.price, quote.previousSettlement)}
                    disabled={!ask?.price}
                    onClick={() => fill(ask?.price?.text)}
                  >
                    <span>{t("卖{n}", { n: level + 1 })}</span>
                    <span>{ask?.price?.text ?? "—"}</span>
                    <span>{ask?.price ? String(ask.lots ?? "") : ""}</span>
                  </button>
                );
              })}
              {[0, 1, 2, 3, 4].map(level => {
                const bid = quote.bids[level];
                return (
                  <button
                    key={`bid${level}`}
                    type="button"
                    className="order-bid"
                    data-first-bid={level === 0 || undefined}
                    data-tone={priceTone(bid?.price, quote.previousSettlement)}
                    disabled={!bid?.price}
                    onClick={() => fill(bid?.price?.text)}
                  >
                    <span>{t("买{n}", { n: level + 1 })}</span>
                    <span>{bid?.price?.text ?? "—"}</span>
                    <span>{bid?.price ? String(bid.lots ?? "") : ""}</span>
                  </button>
                );
              })}
            </>
          ) : (
            <p className="subtle">{t("无行情")}</p>
          )}
        </div>
        <div className="source-actions">
          {[Side.BUY, Side.SELL].map(side => (
            <button
              key={side}
              type="button"
              className={side === Side.BUY ? "order-buy" : "order-sell"}
              data-suggested={suggested === side || undefined}
              disabled={pending !== undefined}
              onClick={event => submit(side, event.currentTarget.form)}
            >
              {t(side === Side.BUY ? "买入" : "卖出")} <small>{accountName}</small>
            </button>
          ))}
        </div>
        {pending !== undefined && (
          <div className="order-confirm" role="alertdialog" aria-label={t("确认委托")}>
            <p>
              {t("{side} {contract} {offset} {lots} 手，限价 {price}，账户 {account}", {
                side: t(pending === Side.BUY ? "买入" : "卖出"),
                contract: `${exchange} · ${symbol}`,
                offset: t(offsetLabels[offset]),
                lots: order.quantity,
                price: order.price,
                account: accountName,
              })}
            </p>
            <button
              type="button"
              className="danger"
              onClick={event => submit(pending, event.currentTarget.form)}
            >
              {t("确认发送")}
            </button>
            <button type="button" onClick={() => setPending(undefined)}>
              {t("取消")}
            </button>
          </div>
        )}
      </fieldset>
    </form>
  );
}
