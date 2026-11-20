import type { Decimal } from "../../src/generated/asterion/common_pb";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import { OrderState, type Position, type Session } from "../../src/generated/asterion/trading_pb";
import { key } from "./live-shared";

// Display only: Trading enforces every limit itself, also counting unconfirmed
// orders by their own lots, which the session does not carry.
export const lots = (value: Decimal | undefined) => Number(value?.text || "0");
const working = [OrderState.SUBMITTED, OrderState.ACCEPTED, OrderState.PARTIALLY_FILLED];

// Lots each contract takes against the position limits: positions on both
// sides and the unfilled rest of working opening orders.
export function contractLots(live: Session) {
  const held = new Map<string, number>();
  const add = (contract: string, quantity: number) =>
    held.set(contract, (held.get(contract) ?? 0) + quantity);
  for (const position of live.positions)
    add(key(position.instrument), lots(position.todayLots) + lots(position.yesterdayLots));
  for (const order of live.workingOrders)
    if (order.offset === Offset.OPEN)
      add(key(order.instrument), lots(order.lots) - lots(order.filledLots));
  return held;
}

// The largest opening order of a contract the order and position limits allow now.
export function openableLots(live: Session, contract: string) {
  const limits = live.policy?.limits;
  if (live.workingOrders.length + live.unconfirmed.length >= (limits?.maxWorkingOrders ?? 0))
    return 0;
  const held = contractLots(live);
  const gross = [...held.values()].reduce((sum, value) => sum + value, 0);
  return Math.max(
    0,
    Math.min(
      lots(limits?.maxOrderLots),
      lots(limits?.maxContractLots) - (held.get(contract) ?? 0),
      lots(limits?.maxGrossLots) - gross,
    ),
  );
}

// A position's lots free to close: those working close orders do not already take.
// Where the exchange keeps today's and earlier lots apart, each closes separately.
export function closableLots(live: Session, position: Position, apart: boolean) {
  const today = lots(position.todayLots);
  const yesterday = lots(position.yesterdayLots);
  // Closing a long sells; closing a short buys.
  const closing = position.side === Side.BUY ? Side.SELL : Side.BUY;
  const closes = live.workingOrders.filter(
    order =>
      order.id &&
      working.includes(order.state) &&
      key(order.instrument) === key(position.instrument) &&
      order.side === closing &&
      order.offset !== Offset.OPEN,
  );
  const held = (today: boolean) =>
    closes
      .filter(order => (order.offset === Offset.CLOSE_TODAY) === today)
      .reduce((sum, order) => sum + lots(order.lots) - lots(order.filledLots), 0);
  const closableToday = Math.max(0, today - held(true));
  const closableYesterday = Math.max(0, yesterday - held(false));
  return {
    closing,
    today: closableToday,
    yesterday: closableYesterday,
    total: apart
      ? closableToday + closableYesterday
      : Math.max(0, today + yesterday - held(true) - held(false)),
  };
}
