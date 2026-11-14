import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { FeedState } from "../../generated/asterion/market_pb";
import { warehouseData } from "../../bridge/warehouse-data";
import { readySnapshot } from "../../testing/core";
import { useMarket, useMarketOpenError, type MarketView } from "../market";
import { BackendError } from "../../i18n/errors";
import { StatusIndicators } from "./StatusIndicators";

vi.mock("../market", () => ({
  useMarket: vi.fn(() => null),
  useMarketOpenError: vi.fn(() => ""),
}));
vi.mock("../../bridge/warehouse-data", () => ({
  warehouseData: vi.fn(() => new Promise(() => {})),
}));

const show = (
  snapshot = readySnapshot(),
  openMarketSettings = vi.fn(),
  openWarehouseSettings = vi.fn(),
) => (
  <StatusIndicators
    snapshot={snapshot}
    openMarketSettings={openMarketSettings}
    openMarket={() => {}}
    openWarehouseSettings={openWarehouseSettings}
    openTradingSettings={() => {}}
    openTrading={() => {}}
  />
);

test("indicators follow the sidebar: market, research, warehouse, then each trading connection", () => {
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
    "● 行情 未连接",
    "● 研究",
    "● 数据 连接中",
    "● 交易·simnow 连接中",
  ]);
  expect(screen.getByText("● 研究").dataset.tone).toBe("ok");
  expect(screen.getByRole("button", { name: "● 交易·simnow 连接中" })).toBeTruthy();

  snapshot.lab = { ...snapshot.lab!, state: "restarting" };
  view.rerender(show(structuredClone(snapshot)));
  expect(screen.getByText("● 研究 重启中").dataset.tone).toBe("danger");
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

test("the market indicator shows the service's own link to the broker and why it failed", () => {
  const market = (state: FeedState, online = true, error?: string) =>
    ({ online, error, board: { feed: { state } } }) as unknown as MarketView;
  vi.mocked(useMarket).mockReturnValue(market(FeedState.FAILED));
  const view = render(show());
  expect(screen.getByText("● 行情 柜台未连接").dataset.tone).toBe("danger");

  vi.mocked(useMarket).mockReturnValue(market(FeedState.LOGGING_IN));
  view.rerender(show());
  expect(screen.getByText("● 行情 连接柜台中")).toBeTruthy();

  vi.mocked(useMarket).mockReturnValue(market(FeedState.CONNECTED));
  view.rerender(show());
  expect(screen.getByText("● 行情").dataset.tone).toBe("ok");

  vi.mocked(useMarket).mockReturnValue(market(FeedState.CONNECTED, false, "connection refused"));
  view.rerender(show());
  expect(screen.getByText("● 行情 中断").title).toBe("connection refused");
  vi.mocked(useMarket).mockReturnValue(null);
});

test("the warehouse indicator reads the service and says why it failed", async () => {
  vi.mocked(warehouseData).mockResolvedValueOnce({} as never);
  const view = render(show());
  await waitFor(() => expect(screen.getByText("● 数据").dataset.tone).toBe("ok"));
  view.unmount();

  vi.mocked(warehouseData).mockRejectedValueOnce(new Error("connection refused"));
  render(show());
  await waitFor(() => expect(screen.getByText("● 数据 中断").title).toBe("connection refused"));
});

test("failed initial connections have the same danger state for Market and Warehouse", async () => {
  vi.mocked(useMarketOpenError).mockReturnValue(
    new BackendError("conflict", "market protocol differs from this Terminal"),
  );
  vi.mocked(warehouseData).mockRejectedValueOnce(
    new BackendError("conflict", "warehouse protocol differs from this Terminal"),
  );
  const view = render(show());
  expect(screen.getByText("● 行情 中断").dataset.tone).toBe("danger");
  expect(screen.getByText("● 行情 中断").title).toBe(
    "行情服务与 Terminal 的通信协议不一致，请将两边更新到同一版本。",
  );
  await waitFor(() => expect(screen.getByText("● 数据 中断").dataset.tone).toBe("danger"));
  expect(screen.getByText("● 数据 中断").title).toBe(
    "数据服务与 Terminal 的通信协议不一致，请将两边更新到同一版本。",
  );

  vi.mocked(useMarketOpenError).mockReturnValue("");
  view.rerender(show());
  expect(screen.getByText("● 行情 未连接").dataset.tone).toBeUndefined();
  expect(screen.getByText("● 行情 未连接").title).toBe("");
});

test("an unset Warehouse opens its settings", () => {
  const snapshot = readySnapshot();
  snapshot.warehouse_endpoint = null;
  const openWarehouseSettings = vi.fn();
  render(show(snapshot, vi.fn(), openWarehouseSettings));
  fireEvent.click(screen.getByRole("button", { name: "● 数据 未设置" }));
  expect(openWarehouseSettings).toHaveBeenCalled();
});
