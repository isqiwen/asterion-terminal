import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../testing/core";
import { StatusIndicators } from "./StatusIndicators";

const show = (snapshot = readySnapshot(), openMarketSettings = vi.fn()) => (
  <StatusIndicators
    snapshot={snapshot}
    openMarketSettings={openMarketSettings}
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
  snapshot.market_endpoint = null;
  const openMarketSettings = vi.fn();
  render(show(snapshot, openMarketSettings));
  fireEvent.click(screen.getByRole("button", { name: "● 行情 未设置" }));
  expect(openMarketSettings).toHaveBeenCalled();
  expect(screen.getByRole("button", { name: "● 交易 未设置" })).toBeTruthy();
});
