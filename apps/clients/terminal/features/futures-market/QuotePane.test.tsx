import { act, render, screen } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import { marketData } from "../../src/bridge/market-data";
import {
  BoardSchema,
  FeedState,
  MinutesSchema,
  QuoteRowSchema,
  Subscription,
  type Minutes,
} from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { MarketView } from "../../src/host/market";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { QuotePane, accountPriceLines } from "./QuotePane";
import { Side } from "../../src/generated/asterion/common_pb";
import { OrderState, SessionSchema } from "../../src/generated/asterion/trading_pb";
import { feature } from "./feature";

// The chart draws on a canvas; these tests read the note beneath it.
vi.mock("@asterion/terminal/charts/IntradayChart", () => ({ IntradayChart: () => null }));
vi.mock("../../src/bridge/market-data", () => ({ marketData: vi.fn() }));
registerTerminalFeatures([feature]);

const row = create(QuoteRowSchema, {
  instrument: { exchange: "SHFE", symbol: "rb2610" },
  subscription: Subscription.ACTIVE,
});
// The market service as one quote stream observes the contract.
const service = (generation: number): MarketView => ({
  board: create(BoardSchema, { feed: { state: FeedState.CONNECTED }, rows: [row] }),
  online: true,
  ticks: new Map(),
  generation,
  interrupted: false,
  lastReceivedMs: 0,
});
// What the service answers: the day's series of a contract as it has observed
// it since this hour, Shanghai time.
const answer = (since: number): Minutes => {
  const start = BigInt(Date.UTC(2026, 9, 13, since - 8));
  const price = { text: "3500" };
  return create(MinutesSchema, {
    instrument: { exchange: "SHFE", symbol: "rb2610" },
    tradingDay: "2026-10-13",
    observedFromMs: start,
    previousSettlement: price,
    bars: [
      {
        startMs: start,
        open: price,
        high: price,
        low: price,
        close: price,
        volume: 1n,
        averagePrice: price,
        openInterest: { text: "1" },
      },
    ],
  });
};
let replies: ((series: Minutes) => void)[] = [];
beforeEach(() => {
  replies = [];
  vi.mocked(marketData).mockImplementation(
    () => new Promise(resolve => replies.push(resolve as (series: Minutes) => void)),
  );
});
// The pane on its intraday chart, with every read left for the test to answer.
function mount(market: MarketView) {
  const store = createWorkspaceDrafts();
  const pane = (selected: MarketView) => (
    <WorkspaceDraftScope store={store} namespace="test">
      <QuotePane
        context={{
          snapshot: readySnapshot(),
          busy: false,
          error: "",
          navigate: () => {},
          openSettings: () => {},
          trade: async () => {},
        }}
        market={selected}
        row={row}
        name="螺纹钢"
        pane="test"
        initial="分时"
      />
    </WorkspaceDraftScope>
  );
  const view = render(pane(market));
  return (next: MarketView) => view.rerender(pane(next));
}
const note = () => screen.queryByText(/行情服务观测/)?.textContent ?? null;

test("a restarted quote stream's series is asked for afresh and a late reply is dropped", async () => {
  const select = mount(service(1));
  expect(replies).toHaveLength(1);
  // The stream starts again while the first read has not been answered.
  select(service(2));
  expect(replies).toHaveLength(2);
  await act(async () => replies[0]!(answer(10)));
  expect(note()).toBeNull();
  expect(document.body.textContent).toContain("读取中");
  await act(async () => replies[1]!(answer(11)));
  expect(note()).toContain("11:00");
});

test("account price lines mark open position cost and working orders of the selected contract", () => {
  const live = create(SessionSchema, {
    policy: {
      contracts: [
        {
          instrument: { exchange: "SHFE", symbol: "rb2610" },
          multiplier: { text: "10" },
          priceTick: { text: "1" },
        },
      ],
    },
    positions: [
      {
        instrument: { exchange: "SHFE", symbol: "rb2610" },
        side: Side.BUY,
        todayLots: { text: "2" },
        yesterdayLots: { text: "1" },
        cost: { text: "105300" },
      },
      {
        instrument: { exchange: "SHFE", symbol: "au2612" },
        side: Side.SELL,
        todayLots: { text: "1" },
        yesterdayLots: { text: "0" },
        cost: { text: "600000" },
      },
    ],
    workingOrders: [
      {
        id: "ord-1",
        instrument: { exchange: "SHFE", symbol: "rb2610" },
        side: Side.SELL,
        lots: { text: "3" },
        filledLots: { text: "1" },
        price: { text: "3550" },
        state: OrderState.PARTIALLY_FILLED,
      },
      {
        id: "ord-2",
        instrument: { exchange: "SHFE", symbol: "rb2610" },
        side: Side.BUY,
        lots: { text: "1" },
        filledLots: { text: "1" },
        price: { text: "3480" },
        state: OrderState.FILLED,
      },
    ],
  });
  expect(accountPriceLines({ sim: { live } }, service(1), row)).toEqual([
    { value: "3510.00", label: "多 3", tone: "up", dashed: false },
    { value: "3550", label: "挂单 卖 2", tone: "down", dashed: true },
  ]);

  const productLive = create(SessionSchema, {
    policy: {
      contracts: [
        {
          instrument: { exchange: "SHFE", symbol: "rb" },
          product: "rb",
          deliveryMonth: "",
          multiplier: { text: "10" },
          priceTick: { text: "1" },
        },
      ],
    },
    positions: live.positions,
  });
  expect(
    accountPriceLines(
      {
        "trading-uuid-1": { instance: "simnow-a", live: productLive },
        "trading-uuid-2": { instance: "real-b" },
      },
      service(1),
      row,
    ),
  ).toEqual([{ value: "3510.00", label: "simnow-a 多 3", tone: "up", dashed: false }]);
});
