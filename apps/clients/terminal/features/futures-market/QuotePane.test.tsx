import { act, render, screen } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import { marketData } from "../../src/bridge/market-data";
import {
  MinuteSeriesSchema,
  SnapshotSchema,
  SubscriptionSchema,
  type MinuteSeries,
} from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { MarketView } from "../../src/host/market";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { QuotePane } from "./QuotePane";
import { feature } from "./feature";

// The chart draws on a canvas; these tests read the note beneath it.
vi.mock("@asterion/client-ui/IntradayChart", () => ({ IntradayChart: () => null }));
vi.mock("../../src/bridge/market-data", () => ({ marketData: vi.fn() }));
registerTerminalFeatures([feature]);

const row = create(SubscriptionSchema, {
  instrument: { venue: "SHFE", symbol: "rb2610" },
  state: "subscribed",
});
// The market service as one of its processes observes the contract.
const service = (instance: string): MarketView => ({
  snapshot: create(SnapshotSchema, {
    instanceId: instance,
    phase: "connected",
    subscriptions: [row],
  }),
  online: true,
  ticks: [],
  generation: 1,
  interrupted: false,
});
// What the service answers: the day's series of a contract as it has observed
// it since this hour, Shanghai time.
const answer = (since: number, symbol = "rb2610"): MinuteSeries => {
  const start = BigInt(Date.UTC(2026, 9, 13, since - 8));
  return create(MinuteSeriesSchema, {
    instrument: { venue: "SHFE", symbol },
    tradingDay: "20261013",
    firstObservationMs: start,
    previousSettlement: "3500",
    bars: [
      {
        startMs: start,
        open: "3500",
        high: "3500",
        low: "3500",
        close: "3500",
        volume: 1n,
        averagePrice: "3500",
        openInterest: "1",
      },
    ],
  });
};
let replies: ((series: MinuteSeries) => void)[] = [];
beforeEach(() => {
  replies = [];
  vi.mocked(marketData).mockImplementation(
    () => new Promise(resolve => replies.push(resolve as (series: MinuteSeries) => void)),
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

test("a restarted market service's series is asked for afresh and a late reply is dropped", async () => {
  const select = mount(service("a"));
  expect(replies).toHaveLength(1);
  // The service starts again while the first read has not been answered.
  select(service("b"));
  expect(replies).toHaveLength(2);
  await act(async () => replies[0]!(answer(10)));
  expect(note()).toBeNull();
  expect(document.body.textContent).toContain("读取中");
  await act(async () => replies[1]!(answer(11)));
  expect(note()).toContain("11:00");
});

test("a series of another contract is refused", async () => {
  mount(service("a"));
  await act(async () => replies[0]!(answer(10, "rb2611")));
  expect(note()).toBeNull();
  expect(screen.getByRole("alert").textContent).toBe("返回的数据与所选合约不匹配");
});
