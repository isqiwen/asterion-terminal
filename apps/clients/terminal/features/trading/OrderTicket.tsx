import { useEffect, useLayoutEffect, useMemo, useRef, useState, type CSSProperties } from "react";
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

function stepPrice(
  raw: string,
  tickText: string | undefined,
  direction: 1 | -1,
  fallback?: string,
) {
  const tick = tickText?.trim();
  if (!tick || !/^\d+(\.\d+)?$/.test(tick)) return undefined;
  const baseText = raw.trim() || fallback?.trim() || "";
  if (!/^\d+(\.\d+)?$/.test(baseText)) return undefined;
  const decimals = Math.max(
    (tick.split(".")[1] ?? "").length,
    (baseText.split(".")[1] ?? "").length,
  );
  const scale = 10n ** BigInt(decimals);
  const toScaled = (text: string) => {
    const [whole, frac = ""] = text.split(".");
    return BigInt(whole!) * scale + BigInt(frac.padEnd(decimals, "0"));
  };
  const next = toScaled(baseText) + BigInt(direction) * toScaled(tick);
  if (next <= 0n) return undefined;
  const whole = next / scale;
  const frac = (next % scale).toString().padStart(decimals, "0").replace(/0+$/, "");
  return frac ? `${whole}.${frac}` : whole.toString();
}

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
  onContract,
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
  // Told which contract the ticket is on, so the chart beside it can follow.
  onContract?: (contract: string) => void;
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
  const policyContracts = record.session?.policy?.contracts;
  const positions = record.session?.positions;
  const hasProductPolicy = (policyContracts ?? []).some(isProductPolicy);
  const { contracts: listed } = useTradingCatalog(hasProductPolicy ? (connection ?? "") : "");
  const contracts = useMemo(() => {
    const result: ContractTerms[] = [];
    const seen = new Set<string>();
    const addContract = (entry: ContractTerms) => {
      const id = key(entry.instrument);
      if (!seen.has(id)) {
        seen.add(id);
        result.push(entry);
      }
    };
    for (const item of policyContracts ?? []) {
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
      for (const pos of positions ?? []) {
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
    return result;
  }, [policyContracts, listed, positions]);
  const traded = order.contract
    ? contracts.find(item => key(item.instrument) === order.contract)
    : contracts[0];
  const exchange = traded?.instrument?.exchange ?? "";
  const symbol = traded?.instrument?.symbol ?? "";
  const tradedKey = traded ? key(traded.instrument) : "";
  useEffect(() => {
    if (tradedKey) onContract?.(tradedKey);
  }, [tradedKey, onContract]);
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
    const sentContract = key(contractInstrument);
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
          // A row chosen beside the ticket may have moved it to another contract meanwhile.
          if (count > 1n && remaining > 0n)
            setOrder(current =>
              (current.contract || sentContract) === sentContract
                ? { ...current, quantity: String(remaining) }
                : current,
            );
        }
        onSent();
      } finally {
        submittingRef.current = false;
        setSubmitting(false);
      }
    })();
  };
  const locked = busy || submitting;
  const tickText = traded?.priceTick?.text;
  const closableTarget =
    offset === "open"
      ? 0
      : suggested
        ? closable(suggested === Side.SELL ? Side.BUY : Side.SELL)
        : Math.max(closable(Side.BUY), closable(Side.SELL));
  const stepBy = (direction: 1 | -1) => {
    const next = stepPrice(
      order.price,
      tickText,
      direction,
      quoteUsable ? quote?.last?.text : undefined,
    );
    if (next) setOrder(current => ({ ...current, price: next }));
  };
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
                  {order.contract ? order.contract.replace(".", " · ") : t("请选择合约")}
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
            <span className="ticket-stepper">
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
              {tickText && (
                <>
                  <button
                    type="button"
                    aria-label={t("减一跳")}
                    title={t("减一跳")}
                    disabled={!order.price.trim() && !(quoteUsable && quote?.last?.text)}
                    onClick={() => stepBy(-1)}
                  >
                    −
                  </button>
                  <button
                    type="button"
                    aria-label={t("加一跳")}
                    title={t("加一跳")}
                    disabled={!order.price.trim() && !(quoteUsable && quote?.last?.text)}
                    onClick={() => stepBy(1)}
                  >
                    +
                  </button>
                </>
              )}
            </span>
          </label>
          <div className="ticket-quick-lots" role="group" aria-label={t("快捷手数")}>
            {["1", "5", "10"].map(lots => (
              <button
                key={lots}
                type="button"
                aria-pressed={order.quantity === lots}
                onClick={() => setOrder(current => ({ ...current, quantity: lots }))}
              >
                {lots}
              </button>
            ))}
            {closableTarget > 0 && (
              <button
                type="button"
                aria-pressed={order.quantity === String(closableTarget)}
                onClick={() =>
                  setOrder(current => ({ ...current, quantity: String(closableTarget) }))
                }
              >
                {t("全部可平")}
              </button>
            )}
          </div>
        </div>
        {!traded && order.contract && (
          <p className="alert" role="alert">
            {t("合约 {contract} 不在此账户的可交易合约中", { contract: order.contract })}
          </p>
        )}
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
              disabled={!traded}
              onClick={event => submit(side, event.currentTarget.form)}
            >
              {t(side === Side.BUY ? "买入" : "卖出")}{" "}
              <small>{symbol ? `${symbol} · ${accountName}` : accountName}</small>
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
