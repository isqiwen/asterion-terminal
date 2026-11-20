import { useEffect, useLayoutEffect, useRef, useState, type CSSProperties } from "react";
import { useMarket } from "../../src/host/market";
import { decimal, decimalText, priceTone } from "../../src/bridge/decimal";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import type { Account, ContractTerms } from "../../src/generated/asterion/trading_pb";
import { FeedState, Subscription, type QuoteRow } from "../../src/generated/asterion/market_pb";
import type { MarketView } from "../../src/host/market";
import type { TerminalContext } from "../../src/host/feature-registry";
import { explicitCloseBuckets } from "../../src/ui/closePolicy";
import { closableLots, openableLots } from "./exposure";
import {
  command,
  isProductPolicy,
  key,
  rowProduct,
  t,
  useTradingCatalog,
  type Run,
} from "./live-shared";

const offsetValues = {
  open: Offset.OPEN,
  close_today: Offset.CLOSE_TODAY,
  close_yesterday: Offset.CLOSE_YESTERDAY,
  close: Offset.CLOSE,
};

function usableQuote(market: MarketView | null, row: QuoteRow | undefined, now: number) {
  const quote = row?.quote;
  return !!(
    market?.online &&
    market.board.feed?.state === FeedState.CONNECTED &&
    row?.subscription === Subscription.ACTIVE &&
    quote?.exchangeMs !== undefined &&
    Number(quote.exchangeMs) <= now + 5000 &&
    now - Number(quote.exchangeMs) <= 30000 &&
    now - Number(quote.receivedMs) <= 30000
  );
}

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
  connection,
  record,
  snapshot,
  busy,
  accountName,
  confirm,
  run,
  onSent,
  preset,
  onPresetApplied,
  initialContract,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  connection?: string;
  record: Account;
  accountName: string;
  confirm: boolean;
  run: Run;
  onSent: () => void;
  preset?: Preset;
  onPresetApplied?: () => void;
  initialContract?: string;
}) {
  const [now, setNow] = useState(Date.now);
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, []);
  const [pending, setPending] = useState<Side>();
  const [submitting, setSubmitting] = useState(false);
  const submittingRef = useRef(false);
  const formRef = useRef<HTMLFormElement>(null);
  const confirmDialog = useRef<HTMLDialogElement>(null);
  useLayoutEffect(() => {
    if (pending === undefined) return;
    const element = confirmDialog.current;
    element?.showModal();
    return () => element?.close();
  }, [pending]);
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
  const previousInitial = useRef(initialContract);
  useEffect(() => {
    if (!initialContract || previousInitial.current === initialContract) return;
    previousInitial.current = initialContract;
    setPending(undefined);
    setSuggested(undefined);
    setOrder(current => ({
      ...current,
      contract: initialContract,
      price: current.contract === initialContract ? current.price : "",
    }));
  }, [initialContract]);
  const [suggested, setSuggested] = useState<Side>();
  const policyContracts = record.session?.policy?.contracts ?? [];
  const hasProductPolicy = policyContracts.some(isProductPolicy);
  const { contracts: listed } = useTradingCatalog(hasProductPolicy ? (connection ?? "") : "");
  const contracts: ContractTerms[] = [];
  const seenContracts = new Set<string>();
  const addContract = (entry: ContractTerms) => {
    const id = key(entry.instrument);
    if (!seenContracts.has(id)) {
      seenContracts.add(id);
      contracts.push(entry);
    }
  };
  for (const item of policyContracts) {
    if (!isProductPolicy(item)) {
      addContract(item);
      continue;
    }
    const exchange = item.instrument?.exchange ?? "";
    for (const row of listed) {
      if (row.instrument?.exchange === exchange && rowProduct(row) === item.product) {
        addContract({ ...item, instrument: row.instrument });
      }
    }
    for (const pos of record.session?.positions ?? []) {
      const sym = pos.instrument?.symbol ?? "";
      if (
        pos.instrument?.exchange === exchange &&
        sym.startsWith(item.product) &&
        /^\d{3,4}$/.test(sym.slice(item.product.length))
      ) {
        addContract({ ...item, instrument: pos.instrument });
      }
    }
  }
  const traded = order.contract
    ? contracts.find(item => key(item.instrument) === order.contract)
    : contracts[0];
  const exchange = traded?.instrument?.exchange ?? "";
  const symbol = traded?.instrument?.symbol ?? "";
  const explicitBuckets = explicitCloseBuckets(snapshot, exchange);
  const session = record.session;
  // Lots of the contract's long or short position free to close now.
  const closable = (side: Side) => {
    const position = session?.positions.find(
      item => traded && key(item.instrument) === key(traded.instrument) && item.side === side,
    );
    return position && session ? closableLots(session, position, explicitBuckets).total : 0;
  };
  const offset =
    order.offset === "open"
      ? "open"
      : explicitBuckets
        ? order.offset === "close"
          ? "close_today"
          : order.offset
        : "close";
  const market = useMarket();
  const rows = market?.board.rows;
  const quoteRow = rows?.find(row => key(row.instrument) === key(traded?.instrument));
  const quote = quoteRow?.quote;
  const quoteUsable = usableQuote(market, quoteRow, now);
  // A preset applies once; selling takes the best bid and buying the best ask, if quoted.
  const applied = useRef<Preset>(undefined);
  useEffect(() => {
    if (!preset || applied.current === preset) return;
    applied.current = preset;
    const row = rows?.find(row => key(row.instrument) === preset.contract);
    const book = usableQuote(market, row, now) ? row?.quote : undefined;
    const price = (preset.side === Side.SELL ? book?.bids[0] : book?.asks[0])?.price?.text;
    setPending(undefined);
    setSuggested(preset.side);
    setOrder({ ...preset, price: price ?? "" });
    onPresetApplied?.();
  }, [preset, market, rows, now, onPresetApplied]);
  const offsetLabels = {
    open: "开仓",
    close_today: "平今",
    close_yesterday: "平昨",
    close: "平仓",
  };
  const maxLots = Number(decimalText(record.session?.policy?.limits?.maxOrderLots));
  const totalLots = /^[1-9][0-9]*$/.test(order.quantity) ? BigInt(order.quantity) : 0n;
  const step =
    offset !== "open" && Number.isFinite(maxLots) && maxLots >= 1
      ? BigInt(Math.floor(maxLots))
      : totalLots;
  const count = step > 0n ? (totalLots + step - 1n) / step : 1n;
  const fill = (price: string | undefined) => price && setOrder(current => ({ ...current, price }));
  const submit = (side: Side, form: HTMLFormElement | null) => {
    if (submittingRef.current || !form?.reportValidity() || !traded) return;
    if (confirm && pending !== side) return setPending(side);
    setPending(undefined);
    setSuggested(undefined);
    const contractInstrument = traded.instrument;
    const targetOffset = offsetValues[offset];
    const limitPrice = order.price;
    let remaining = totalLots;
    submittingRef.current = true;
    setSubmitting(true);
    void (async () => {
      try {
        while (remaining > 0n) {
          const leg = remaining < step ? remaining : step;
          const sent = await command(run, record, () => ({
            case: "submitOrder",
            value: {
              orderId: crypto.randomUUID(),
              instrument: contractInstrument,
              side,
              offset: targetOffset,
              lots: decimal(String(leg)),
              price: decimal(limitPrice),
            },
          }));
          if (!sent) return;
          remaining -= leg;
          if (count > 1n && remaining > 0n)
            setOrder(current => ({ ...current, quantity: String(remaining) }));
        }
        onSent();
      } finally {
        submittingRef.current = false;
        setSubmitting(false);
      }
    })();
  };
  const locked = busy || submitting;
  const most = Math.max(
    1,
    ...[...(quote?.asks ?? []), ...(quote?.bids ?? [])].map(item => Number(item.lots ?? 0)),
  );
  const depth = (lots: bigint | undefined) =>
    ({ "--depth": `${(Number(lots ?? 0) / most) * 100}%` }) as CSSProperties;
  return (
    <form ref={formRef} aria-label={t("CTP 委托")} onSubmit={event => event.preventDefault()}>
      <fieldset disabled={locked || pending !== undefined}>
        <div className="futures-fields">
          <label>
            {t("合约")}
            <select
              aria-label={t("委托合约")}
              required
              value={traded ? key(traded.instrument) : ""}
              onChange={event => {
                const nextContract = event.target.value;
                setPending(undefined);
                setSuggested(undefined);
                setOrder(current => ({
                  ...current,
                  contract: nextContract,
                  price: nextContract === key(traded?.instrument) ? current.price : "",
                }));
              }}
            >
              {!traded && (
                <option value="" disabled>
                  {order.contract
                    ? t("合约 {contract} 不在此账户的可交易合约中", { contract: order.contract })
                    : t("请选择合约")}
                </option>
              )}
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
              pattern="[1-9][0-9]*"
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
              pattern="\s*\d+(\.\d{1,8})?\s*"
              value={order.price}
              placeholder={
                traded ? t("最小变动 {tick}", { tick: decimalText(traded.priceTick) }) : ""
              }
              onChange={event => setOrder({ ...order, price: event.target.value })}
              required
            />
          </label>
        </div>
        {traded && session && (
          <p className="ticket-capacity" aria-label={t("可开与可平手数")}>
            {[
              t("可开 {n}", { n: openableLots(session, key(traded.instrument)) }),
              t("可平多 {n}", { n: closable(Side.BUY) }),
              t("可平空 {n}", { n: closable(Side.SELL) }),
            ].join(" · ")}
          </p>
        )}
        {quote && (
          <p role="status" className={quoteUsable ? "subtle" : "alert"}>
            {quote.exchangeMs === undefined
              ? t("报价时间未知")
              : t("报价时间 {time}", {
                  time: new Date(Number(quote.exchangeMs)).toLocaleTimeString("sv-SE", {
                    timeZone: "Asia/Shanghai",
                    hour12: false,
                  }),
                })}
            {!quoteUsable && ` · ${t("报价已过期")}`}
          </p>
        )}
        <div className="order-book" role="group" aria-label={t("五档行情")}>
          {quote ? (
            <>
              <button
                type="button"
                className="order-last"
                disabled={!quoteUsable || !quote.last}
                onClick={() => fill(quote.last?.text)}
              >
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
                    style={depth(ask?.lots)}
                    disabled={!quoteUsable || !ask?.price}
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
                    style={depth(bid?.lots)}
                    disabled={!quoteUsable || !bid?.price}
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
              onClick={event => submit(side, event.currentTarget.form)}
            >
              {t(side === Side.BUY ? "买入" : "卖出")} <small>{accountName}</small>
            </button>
          ))}
        </div>
      </fieldset>
      {pending !== undefined && (
        <dialog
          ref={confirmDialog}
          className="order-confirm"
          role="alertdialog"
          aria-label={t("确认委托")}
          onCancel={event => {
            event.preventDefault();
            if (!locked) setPending(undefined);
          }}
          onClose={event => {
            if (!event.currentTarget.open && !locked) setPending(undefined);
          }}
        >
          <p>
            {count > 1n
              ? t(
                  "{side} {contract} {offset} {lots} 手（拆分为 {count} 笔），限价 {price}，账户 {account}",
                  {
                    side: t(pending === Side.BUY ? "买入" : "卖出"),
                    contract: `${exchange} · ${symbol}`,
                    offset: t(offsetLabels[offset]),
                    lots: order.quantity,
                    count: String(count),
                    price: order.price,
                    account: accountName,
                  },
                )
              : t("{side} {contract} {offset} {lots} 手，限价 {price}，账户 {account}", {
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
            disabled={locked}
            onClick={() => submit(pending, formRef.current)}
          >
            {t("确认发送")}
          </button>
          <button type="button" autoFocus disabled={locked} onClick={() => setPending(undefined)}>
            {t("取消")}
          </button>
        </dialog>
      )}
    </form>
  );
}
