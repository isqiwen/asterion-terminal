import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { localService, readySnapshot } from "../../testing/core";
import { StatusIndicators } from "./StatusIndicators";

const show = (snapshot = readySnapshot(), openSources = vi.fn()) => (
  <StatusIndicators
    snapshot={snapshot}
    openSources={openSources}
    openTradingSettings={() => {}}
    openTrading={() => {}}
  />
);

test("行情, 研究 and each trading connection each have their own indicator", () => {
  const snapshot = readySnapshot();
  localService(snapshot, "lab").health = "ready";
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
  expect(screen.getByText("● 行情 未连接")).toBeTruthy();
  expect(screen.getByText("● 研究").className).toContain("good");
  expect(screen.getByRole("button", { name: "● 交易·simnow 连接中" })).toBeTruthy();

  localService(snapshot, "lab").state = "restarting";
  view.rerender(show(structuredClone(snapshot)));
  expect(screen.getByText("● 研究 重启中").className).toContain("bad");
});

test("an unset Market opens its settings; no trading connection says so", () => {
  const snapshot = readySnapshot();
  snapshot.market_endpoint = null;
  const openSources = vi.fn();
  render(show(snapshot, openSources));
  fireEvent.click(screen.getByRole("button", { name: "● 行情 未设置" }));
  expect(openSources).toHaveBeenCalled();
  expect(screen.getByRole("button", { name: "● 交易 未设置" })).toBeTruthy();
});
