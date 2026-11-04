import { act, render, screen } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, beforeEach, expect, test, vi } from "vitest";
import { marketData } from "../../src/bridge/market-data";
import {
  BoardSchema,
  FeedState,
  StatusSchema,
  type Status,
} from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { useMarket, type MarketView } from "../../src/host/market";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { RecordingStatus } from "./RecordingStatus";
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
const reply = (recording = {}) =>
  create(StatusSchema, {
    feed: { state: FeedState.CONNECTED },
    recording: { rows: 1234n, ...recording },
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
  const rendered = render(<RecordingStatus context={context} />);
  return (next: MarketView | null) => {
    vi.mocked(useMarket).mockReturnValue(next);
    rendered.rerender(<RecordingStatus context={{ ...context }} />);
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
  expect(screen.getByText("连接行情服务后查看录制状态。")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(3);
});

test("write failures preserve exact counts", async () => {
  vi.useFakeTimers();
  const select = mount(market());
  await act(async () =>
    replies[0]!(
      reply({
        lostRows: 9007199254740993n,
        writeError: "fixture disk full",
        mergeError: "fixture merge failed",
      }),
    ),
  );
  const alerts = () =>
    screen
      .getAllByRole("alert")
      .map(item => item.textContent)
      .join(" ");
  expect(alerts()).toContain("9,007,199,254,740,993");
  expect(alerts()).toContain("fixture merge failed");
  expect(screen.getByText("行情已连接")).toBeDefined();
  select(market(false));
  expect(screen.getByText("行情服务失联，录制状态未知。")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(1);
});
