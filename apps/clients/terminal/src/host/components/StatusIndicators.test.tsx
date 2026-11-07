import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { FeedState } from "../../generated/asterion/quotes_pb";
import { readySnapshot } from "../../testing/core";
import { useMarket, type MarketView } from "../market";
import { StatusIndicators } from "./StatusIndicators";

vi.mock("../market", () => ({ useMarket: vi.fn(() => null) }));

const show = (snapshot = readySnapshot(), openMarketSettings = vi.fn()) => (
  <StatusIndicators
    snapshot={snapshot}
    openMarketSettings={openMarketSettings}
    openMarket={() => {}}
    openTradingSettings={() => {}}
    openTrading={() => {}}
  />
);

test("indicators show research first, then market and each trading connection", () => {
  const snapshot = readySnapshot();
  snapshot.trading_connections = [
    {
      id: "sim",
      host: "h",
      port: 7460,
      instance: "simnow",
      fingerprint: "f",
      revision: "r",
    },
  ];
  const view = render(show(snapshot));
  expect([...view.container.children].map(item => item.textContent)).toEqual([
    "● 研究",
    "● 行情 未连接",
    "● 交易·simnow 连接中",
  ]);
  expect(screen.getByText("● 研究").className).toContain("good");
  expect(screen.getByRole("button", { name: "● 交易·simnow 连接中" })).toBeTruthy();

  snapshot.lab = { ...snapshot.lab!, state: "restarting" };
  view.rerender(show(structuredClone(snapshot)));
  expect(screen.getByText("● 研究 重启中").className).toContain("bad");
});

test("an unset Market opens its settings; no trading connection says so", () => {
  const snapshot = readySnapshot();
  snapshot.quotes_endpoint = null;
  const openMarketSettings = vi.fn();
  render(show(snapshot, openMarketSettings));
  fireEvent.click(screen.getByRole("button", { name: "● 行情 未设置" }));
  expect(openMarketSettings).toHaveBeenCalled();
  expect(screen.getByRole("button", { name: "● 交易 未设置" })).toBeTruthy();
});

test("the market indicator shows the service's own link to the broker and why it failed", () => {
  const market = (state: FeedState, online = true, error?: string) =>
    ({ online, error, board: { feed: { state } } }) as unknown as MarketView;
  vi.mocked(useMarket).mockReturnValue(market(FeedState.FAILED));
  const view = render(show());
  expect(screen.getByText("● 行情 柜台未连接").className).toContain("bad");

  vi.mocked(useMarket).mockReturnValue(market(FeedState.LOGGING_IN));
  view.rerender(show());
  expect(screen.getByText("● 行情 连接柜台中")).toBeTruthy();

  vi.mocked(useMarket).mockReturnValue(market(FeedState.CONNECTED));
  view.rerender(show());
  expect(screen.getByText("● 行情").className).toContain("good");

  vi.mocked(useMarket).mockReturnValue(market(FeedState.CONNECTED, false, "connection refused"));
  view.rerender(show());
  expect(screen.getByText("● 行情 中断").title).toBe("connection refused");
  vi.mocked(useMarket).mockReturnValue(null);
});
