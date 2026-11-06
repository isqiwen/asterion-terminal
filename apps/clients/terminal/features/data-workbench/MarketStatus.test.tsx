import { act, render, screen } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, beforeEach, expect, test, vi } from "vitest";
import { marketData } from "../../src/bridge/market-data";
import {
  BoardSchema,
  FeedState,
  StatusSchema,
  type Status,
} from "../../src/generated/asterion/history_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { useMarket, type MarketView } from "../../src/host/market";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { MarketStatus } from "./MarketStatus";
import { feature } from "./feature";

vi.mock("../../src/bridge/market-data", () => ({ marketData: vi.fn() }));
vi.mock("../../src/host/market", () => ({ useMarket: vi.fn() }));
registerTerminalFeatures([feature]);
afterEach(() => vi.useRealTimers());

// The open Market stream, as the page holds it.
const market = (online = true): MarketView => ({
  board: create(BoardSchema, { feed: { state: FeedState.CONNECTED } }),
  online,
  ticks: [],
  generation: 1,
  interrupted: false,
});
const reply = (failedSubscriptions = 0) =>
  create(StatusSchema, {
    feed: { state: FeedState.CONNECTED },
    subscribed: 1234,
    failedSubscriptions,
  });
let replies: ((status: Status) => void)[] = [];
beforeEach(() => {
  replies = [];
  vi.mocked(marketData).mockReset();
  vi.mocked(marketData).mockImplementation(
    () => new Promise(resolve => replies.push(resolve as (status: Status) => void)),
  );
});

function mount(view: MarketView | null) {
  vi.mocked(useMarket).mockReturnValue(view);
  const context: TerminalContext = {
    snapshot: readySnapshot(),
    busy: false,
    error: "",
    trade: async () => {},
    navigate: () => {},
    openSettings: () => {},
  };
  const rendered = render(<MarketStatus context={context} />);
  return (next: MarketView | null) => {
    vi.mocked(useMarket).mockReturnValue(next);
    rendered.rerender(<MarketStatus context={{ ...context }} />);
  };
}

test("status reads do not overlap, and reconnecting or closing a Market discards pending replies", async () => {
  vi.useFakeTimers();
  const select = mount(market());
  expect(marketData).toHaveBeenCalledWith({ case: "getStatus", value: {} });
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(1);
  select(market(false));
  select(market());
  expect(replies).toHaveLength(2);
  await act(async () => replies[0]!(reply()));
  expect(screen.queryByText("1,234")).toBeNull();
  await act(async () => replies[1]!(reply()));
  expect(screen.getByText("1,234")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(3000));
  expect(replies).toHaveLength(3);
  select(null);
  await act(async () => replies[2]!(reply()));
  expect(screen.queryByText("1,234")).toBeNull();
  expect(screen.getByText("连接行情服务后查看状态。")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(3);
});

test("a lost service stops showing its status", async () => {
  vi.useFakeTimers();
  const select = mount(market());
  await act(async () => replies[0]!(reply()));
  expect(screen.getByText("行情已连接")).toBeDefined();
  select(market(false));
  expect(screen.getByText("行情服务失联，状态未知。")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(1);
});

test("contracts the feed refused are counted", async () => {
  vi.useFakeTimers();
  mount(market());
  await act(async () => replies[0]!(reply(3)));
  expect(screen.getByRole("alert").textContent).toContain("合约订阅失败，每 5 分钟重试: 3");
});
