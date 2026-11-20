import { create } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { OrderTicket } from "./OrderTicket";
import { feature } from "./feature";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { AccountSchema, OrderState } from "../../src/generated/asterion/trading_pb";
import type { TradingCall } from "../../src/bridge/trading";
import { useMarket } from "../../src/host/market";
import { BoardSchema, FeedState, Subscription } from "../../src/generated/asterion/market_pb";
import { Offset, Side } from "../../src/generated/asterion/common_pb";

vi.mock("../../src/host/market", () => ({ useMarket: vi.fn() }));
beforeEach(() => vi.mocked(useMarket).mockReturnValue(null));

registerTerminalFeatures([feature]);

test("disconnected or aged quotes cannot fill prices while manual limit orders remain available", async () => {
  vi.useFakeTimers();
  const now = Date.now();
  const market = {
    board: create(BoardSchema, {
      feed: { state: FeedState.CONNECTED },
      rows: [
        {
          instrument: { exchange: "SHFE", symbol: "rb2701" },
          subscription: Subscription.ACTIVE,
          quote: {
            last: { text: "3000" },
            exchangeMs: BigInt(now),
            receivedMs: BigInt(now),
            bids: [{ price: { text: "2999" } }],
            asks: [{ price: { text: "3001" } }],
          },
        },
      ],
    }),
    online: true,
    ticks: new Map(),
    generation: 1,
    interrupted: false,
    lastReceivedMs: now,
  };
  vi.mocked(useMarket).mockImplementation(() => market);
  const record = create(AccountSchema, {
    id: "sim",
    session: {
      policyRevision: "p1",
      policy: {
        contracts: [{ instrument: { exchange: "SHFE", symbol: "rb2701" } }],
        limits: { maxOrderLots: { text: "5" } },
      },
    },
  });
  const run = vi.fn(async () => true);
  const props = {
    record,
    snapshot: null,
    busy: false,
    accountName: "sim",
    confirm: false,
    run,
    onSent: () => {},
  };
  const page = render(<OrderTicket {...props} />);
  const price = screen.getByRole("textbox", { name: "限价" }) as HTMLInputElement;
  const bid = () => screen.getByRole("button", { name: /买1/ }) as HTMLButtonElement;
  fireEvent.click(bid());
  expect(price.value).toBe("2999");
  market.online = false;
  page.rerender(<OrderTicket {...props} />);
  expect(bid().disabled).toBe(true);
  expect(screen.getByRole("status").textContent).toContain("报价已过期");
  market.online = true;
  page.rerender(<OrderTicket {...props} />);
  expect(bid().disabled).toBe(false);
  await act(() => vi.advanceTimersByTimeAsync(31000));
  expect(bid().disabled).toBe(true);
  page.rerender(
    <OrderTicket
      {...props}
      preset={{ contract: "SHFE.rb2701", offset: "close_today", quantity: "1", side: Side.SELL }}
    />,
  );
  expect(price.value).toBe("");
  fireEvent.click(bid());
  expect(price.value).toBe("");
  fireEvent.change(price, { target: { value: "2998" } });
  await act(async () => fireEvent.click(screen.getByRole("button", { name: /卖出/ })));
  expect(run).toHaveBeenCalledOnce();
});

test("the contract selected from Market is the contract sent by the ticket", async () => {
  const record = create(AccountSchema, {
    id: "sim",
    session: {
      policyRevision: "p1",
      policy: {
        contracts: ["au2612", "rb2701"].map(symbol => ({
          instrument: { exchange: "SHFE", symbol },
          priceTick: { text: "1" },
        })),
        limits: { maxOrderLots: { text: "5" } },
      },
    },
  });
  const operations: TradingCall[] = [];
  const run = vi.fn(async (operation: TradingCall | (() => TradingCall)) => {
    operations.push(typeof operation === "function" ? operation() : operation);
    return true;
  });
  render(
    <OrderTicket
      record={record}
      snapshot={null}
      busy={false}
      accountName="sim"
      confirm={false}
      run={run}
      onSent={() => {}}
      initialContract="SHFE.rb2701"
    />,
  );
  fireEvent.change(screen.getByRole("textbox", { name: "限价" }), { target: { value: "3000" } });
  fireEvent.click(screen.getByRole("button", { name: /买入/ }));
  await waitFor(() => expect(operations).toHaveLength(1));
  const sent = operations[0];
  expect(
    sent.case === "sendCommand" &&
      sent.value.action?.case === "submitOrder" &&
      sent.value.action.value.instrument?.symbol,
  ).toBe("rb2701");
});

test("an unavailable requested contract is not replaced by the first allowed one", () => {
  const run = vi.fn();
  const record = create(AccountSchema, {
    session: {
      policy: {
        contracts: [{ instrument: { exchange: "SHFE", symbol: "au2612" } }],
      },
    },
  });
  render(
    <OrderTicket
      record={record}
      snapshot={null}
      busy={false}
      accountName="sim"
      confirm={false}
      run={run}
      onSent={() => {}}
      initialContract="SHFE.rb2701"
    />,
  );
  const contract = screen.getByRole("combobox", { name: "委托合约" }) as HTMLSelectElement;
  expect(contract.value).toBe("");
  expect(contract.textContent).toContain("合约 SHFE.rb2701 不在此账户的可交易合约中");
  fireEvent.change(screen.getByRole("textbox", { name: "限价" }), { target: { value: "3000" } });
  fireEvent.click(screen.getByRole("button", { name: /买入/ }));
  expect(run).not.toHaveBeenCalled();
});

test("a large close previews its exact quantity without expanding all legs", () => {
  const record = create(AccountSchema, {
    session: {
      policy: {
        contracts: [{ instrument: { exchange: "DCE", symbol: "m2701" } }],
        limits: { maxOrderLots: { text: "5" } },
      },
    },
  });
  const run = vi.fn();
  render(
    <OrderTicket
      record={record}
      snapshot={null}
      busy={false}
      accountName="sim"
      confirm={true}
      run={run}
      onSent={() => {}}
    />,
  );
  fireEvent.change(screen.getByRole("combobox", { name: "开平仓" }), {
    target: { value: "close_today" },
  });
  fireEvent.change(screen.getByRole("textbox", { name: "委托手数" }), {
    target: { value: "999999999999999999" },
  });
  fireEvent.change(screen.getByRole("textbox", { name: "限价" }), { target: { value: "3000" } });
  fireEvent.click(screen.getByRole("button", { name: /卖出/ }));
  expect(screen.getByRole("alertdialog").textContent).toContain(
    "999999999999999999 手（拆分为 200000000000000000 笔）",
  );
  fireEvent.click(screen.getByRole("button", { name: "取消" }));
  expect(run).not.toHaveBeenCalled();
});

test("the ticket shows how many lots may open and how many each position may close", () => {
  const m = { exchange: "DCE", symbol: "m2701" };
  const record = create(AccountSchema, {
    session: {
      policy: {
        contracts: [{ instrument: m }],
        limits: {
          maxOrderLots: { text: "5" },
          maxContractLots: { text: "6" },
          maxGrossLots: { text: "10" },
          maxWorkingOrders: 3,
        },
      },
      positions: [{ instrument: m, side: Side.BUY, todayLots: { text: "2" } }],
      workingOrders: [
        {
          id: "closing",
          instrument: m,
          side: Side.SELL,
          offset: Offset.CLOSE_TODAY,
          state: OrderState.ACCEPTED,
          lots: { text: "1" },
        },
      ],
    },
  });
  render(
    <OrderTicket
      record={record}
      snapshot={null}
      busy={false}
      accountName="sim"
      confirm={false}
      run={vi.fn()}
      onSent={() => {}}
    />,
  );
  // The contract limit leaves 4 of 6; a working close of today's lots takes one long lot.
  expect(screen.getByLabelText("可开与可平手数").textContent).toBe("可开 4 · 可平多 1 · 可平空 0");
});
