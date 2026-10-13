import { act, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { IntradaySeries, LiveMarket, Snapshot } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { QuotePane } from "./QuotePane";
import { plugin } from "./plugin";

// The chart draws on a canvas; these tests read the note beneath it.
vi.mock("@asterion/client-ui/IntradayChart", () => ({ IntradayChart: () => null }));
registerTerminalPlugins([plugin]);

const row: LiveMarket["subscriptions"][number] = {
  venue: "SHFE",
  symbol: "rb2610",
  state: "subscribed",
  error_code: 0,
  quote: null,
};
// Two market services that both observe the contract.
const service = (name: string): LiveMarket => ({
  ...readySnapshot().market!,
  service: `market-${name}`,
  instance_id: `instance-${name}`,
  phase: "connected",
  subscriptions: [row],
});
// What a service answers: the day's series as it has observed it since this
// hour, Shanghai time.
const answer = (market: LiveMarket, since: number): Snapshot => {
  const intraday: IntradaySeries = {
    service: market.service,
    instance_id: market.instance_id,
    venue: row.venue,
    symbol: row.symbol,
    trading_day: "20261013",
    first_observation_ms: Date.UTC(2026, 9, 13, since - 8),
    interrupted: false,
    previous_settlement: "3500",
    bars: [
      {
        start_ms: Date.UTC(2026, 9, 13, since - 8),
        open: "3500",
        high: "3500",
        low: "3500",
        close: "3500",
        volume: 1,
        average_price: "3500",
        open_interest: "1",
      },
    ],
  };
  return { ...readySnapshot(), market, intraday };
};
// The pane on its intraday chart, with every read left for the test to answer.
function mount(market: LiveMarket) {
  const replies: ((result: Snapshot) => void)[] = [];
  const query: TerminalContext["query"] = () => new Promise(resolve => replies.push(resolve));
  const store = createWorkspaceDrafts();
  const pane = (selected: LiveMarket) => (
    <WorkspaceDraftScope store={store} namespace="test">
      <QuotePane
        context={{
          snapshot: { ...readySnapshot(), market: selected },
          busy: false,
          error: "",
          query,
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
  return { replies, select: (next: LiveMarket) => view.rerender(pane(next)) };
}
const note = () => screen.queryByText(/本机行情服务观测/)?.textContent ?? null;

test("another market service's series is asked for afresh and a late reply is dropped", async () => {
  const [first, second] = [service("a"), service("b")];
  const { replies, select } = mount(first);
  expect(replies).toHaveLength(1);
  // The other service is selected while the first has not answered yet.
  select(second);
  expect(replies).toHaveLength(2);
  await act(async () => replies[0](answer(first, 10)));
  expect(note()).toBeNull();
  expect(document.body.textContent).toContain("读取中");
  await act(async () => replies[1](answer(second, 11)));
  expect(note()).toContain("11:00");
  // The same service started again observes from a new beginning.
  select({ ...second, instance_id: "instance-b-again" });
  expect(replies).toHaveLength(3);
  expect(note()).toBeNull();
});

test("a series that names another market service is refused", async () => {
  const [first, second] = [service("a"), service("b")];
  const { replies } = mount(second);
  await act(async () => replies[0](answer(first, 10)));
  expect(note()).toBeNull();
  expect(screen.getByRole("alert").textContent).toBe("返回的数据不属于当前行情服务");
});
