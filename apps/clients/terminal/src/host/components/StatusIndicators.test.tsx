import { create } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { tradingRequest } from "../../bridge/trading";
import { ErrorCode } from "../../generated/asterion/common_pb";
import { FeedState } from "../../generated/asterion/market_pb";
import { BrokerState, ResponseSchema } from "../../generated/asterion/trading_pb";
import { TaskState } from "../../generated/asterion/warehouse_pb";
import { warehouseData } from "../../bridge/warehouse-data";
import { readySnapshot } from "../../testing/core";
import { openPopover } from "../../testing/popover";
import { useMarket, useMarketOpenError, type MarketView } from "../market";
import { TradingAccountsProvider } from "../trading-accounts";
import { WarehouseProvider } from "../warehouse";
import { NotificationCenter } from "../notifications";
import { BackendError } from "../../i18n/errors";
import { StatusIndicators } from "./StatusIndicators";

vi.mock("../../bridge/trading", () => ({
  tradingRequest: vi.fn(() => new Promise(() => {})),
}));

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
  openDataset?: () => void,
) => {
  const endpoint = snapshot?.warehouse_endpoint;
  const key =
    endpoint && "host" in endpoint
      ? `${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`
      : "";
  return (
    <WarehouseProvider endpoint={key}>
      <StatusIndicators
        snapshot={snapshot}
        openMarketSettings={openMarketSettings}
        openMarket={() => {}}
        openWarehouseSettings={openWarehouseSettings}
        openDataset={openDataset}
        openTradingSettings={() => {}}
        openTrading={() => {}}
      />
    </WarehouseProvider>
  );
};

// An indicator by its whole text: its light is an element of its own.
const matches = (text: string) => (_: string, element: Element | null) =>
  element?.matches(".status-item") === true && element.textContent === text;
const indicator = (text: string) => screen.getByText(matches(text));

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
  const services = view.container.querySelector(".status-services")!;
  expect([...services.children].map(item => item.textContent)).toEqual([
    "● 行情 连接中",
    "● 研究",
    "● 数据 连接中",
    "● 交易·simnow 连接中",
  ]);
  expect(indicator("● 研究").dataset.tone).toBe("ok");
  expect(screen.getByRole("button", { name: "● 交易·simnow 连接中" })).toBeTruthy();

  snapshot.lab = { ...snapshot.lab!, state: "restarting" };
  view.rerender(show(structuredClone(snapshot)));
  expect(indicator("● 研究 重启中").dataset.tone).toBe("danger");
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
  expect(indicator("● 行情 柜台未连接").dataset.tone).toBe("danger");

  vi.mocked(useMarket).mockReturnValue(market(FeedState.LOGGING_IN));
  view.rerender(show());
  expect(indicator("● 行情 登录柜台中")).toBeTruthy();

  vi.mocked(useMarket).mockReturnValue(market(FeedState.CONNECTING));
  view.rerender(show());
  expect(indicator("● 行情 连接柜台中")).toBeTruthy();

  vi.mocked(useMarket).mockReturnValue(market(FeedState.RECONNECTING));
  view.rerender(show());
  expect(indicator("● 行情 柜台重连中")).toBeTruthy();

  const timeout = market(FeedState.FAILED);
  timeout.board.feed!.error = "CTP market front connection timed out";
  vi.mocked(useMarket).mockReturnValue(timeout);
  view.rerender(show());
  expect(indicator("● 行情 柜台未连接").title).toContain("超过 30 秒未响应");

  const inactive = market(FeedState.FAILED);
  inactive.board.feed!.ctpError = 8;
  vi.mocked(useMarket).mockReturnValue(inactive);
  view.rerender(show());
  expect(indicator("● 行情 柜台未连接").title).toContain("柜台前置未就绪");

  vi.mocked(useMarket).mockReturnValue(market(FeedState.CONNECTED));
  view.rerender(show());
  expect(indicator("● 行情").dataset.tone).toBe("ok");

  vi.mocked(useMarket).mockReturnValue(market(FeedState.CONNECTED, false, "connection refused"));
  view.rerender(show());
  expect(indicator("● 行情 中断").title).toBe("connection refused");
  vi.mocked(useMarket).mockReturnValue(null);
});

test("the warehouse indicator reads the service and says why it failed", async () => {
  vi.mocked(warehouseData).mockResolvedValueOnce({} as never);
  const view = render(show());
  await waitFor(() => expect(indicator("● 数据").dataset.tone).toBe("ok"));
  view.unmount();

  vi.mocked(warehouseData).mockRejectedValueOnce(new Error("connection refused"));
  render(show());
  await waitFor(() => expect(indicator("● 数据 中断").title).toBe("connection refused"));
});

test("failed initial connections have the same danger state for Market and Warehouse", async () => {
  vi.mocked(useMarketOpenError).mockReturnValue(
    new BackendError("conflict", "market protocol differs from this Terminal"),
  );
  vi.mocked(warehouseData).mockRejectedValueOnce(
    new BackendError("conflict", "warehouse protocol differs from this Terminal"),
  );
  const view = render(show());
  expect(indicator("● 行情 中断").dataset.tone).toBe("danger");
  expect(indicator("● 行情 中断").title).toBe(
    "行情服务与 Terminal 的通信协议不一致，请将两边更新到同一版本。",
  );
  await waitFor(() => expect(indicator("● 数据 中断").dataset.tone).toBe("danger"));
  expect(indicator("● 数据 中断").title).toBe(
    "数据服务与 Terminal 的通信协议不一致，请将两边更新到同一版本。",
  );

  vi.mocked(useMarketOpenError).mockReturnValue("");
  view.rerender(show());
  expect(indicator("● 行情 连接中").dataset.tone).toBeUndefined();
  expect(indicator("● 行情 连接中").title).toBe("");
});

test("an unset Warehouse opens its settings", () => {
  const snapshot = readySnapshot();
  snapshot.warehouse_endpoint = null;
  const openWarehouseSettings = vi.fn();
  render(show(snapshot, vi.fn(), openWarehouseSettings));
  fireEvent.click(screen.getByRole("button", { name: "● 数据 未设置" }));
  expect(openWarehouseSettings).toHaveBeenCalled();
});

test("newly failed Warehouse tasks enter the notification center without repeating old ones", async () => {
  let poll: () => void = () => {};
  const interval = globalThis.setInterval;
  const spy = vi.spyOn(globalThis, "setInterval").mockImplementation((callback, delay, ...args) => {
    if (delay === 5000) poll = callback as () => void;
    return interval(callback, delay, ...args);
  });
  let tasks = [
    { request: { id: "run-1" }, state: TaskState.RUNNING },
    {
      request: { id: "old-failed" },
      state: TaskState.FAILED,
      error: { code: ErrorCode.UNAVAILABLE, message: "gtja: exchange file request failed" },
    },
  ];
  vi.mocked(warehouseData).mockImplementation((async (body: { case: string }) =>
    body.case === "listAcquisitionTasks" ? { tasks, nextId: "" } : {}) as never);

  render(
    <>
      {show()}
      <NotificationCenter />
    </>,
  );
  const trigger = screen.getByRole("button", { expanded: false });
  openPopover(trigger);
  fireEvent.click(screen.getByRole("button", { name: "清空" }));

  await waitFor(() =>
    expect(warehouseData).toHaveBeenCalledWith({
      case: "listAcquisitionTasks",
      value: { afterId: "" },
    }),
  );
  expect(screen.getByText("没有消息")).toBeTruthy();

  tasks = [
    {
      request: { id: "run-1" },
      state: TaskState.FAILED,
      error: { code: ErrorCode.UNAVAILABLE, message: "gtja: exchange file request failed" },
    },
    tasks[1]!,
  ];
  await act(async () => {
    poll();
  });
  const panel = screen.getByRole("region", { name: "通知" });
  await waitFor(() =>
    expect(
      within(panel).getByText(
        "公开数据源连接中断或超时，已取得的数据已入库保留，可稍后重新提交继续补齐。",
      ).title,
    ).toBe("unavailable: gtja: exchange file request failed"),
  );

  await act(async () => {
    poll();
  });
  expect(panel.querySelectorAll("li")).toHaveLength(1);

  spy.mockRestore();
  vi.mocked(warehouseData).mockImplementation(() => new Promise(() => {}));
});

test("active Warehouse acquisition renders centered after the service lights while keeping the Data light steady", async () => {
  const openDataset = vi.fn();
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
  vi.mocked(warehouseData).mockImplementation((async (body: { case: string }) => {
    if (body.case === "getStatus") return { diskFreeBytes: 80n * 1024n ** 3n };
    if (body.case === "listAcquisitionTasks") {
      return {
        tasks: [
          {
            request: { id: "acq-1", table: 3, exchanges: ["SHFE"] },
            state: TaskState.RUNNING,
            items: 210n,
            itemsDone: 126n,
            requestsSent: 45n,
            startedMs: BigInt(Date.now() - 60_000),
          },
          {
            request: { id: "acq-2", table: 4, instruments: ["CFFEX/ic"] },
            state: TaskState.QUEUED,
            items: 0n,
            itemsDone: 0n,
            requestsSent: 0n,
            startedMs: BigInt(Date.now() - 10_000),
          },
        ],
        nextId: "",
      };
    }
    return {};
  }) as never);

  const view = render(show(snapshot, vi.fn(), vi.fn(), openDataset));

  const bar = (await screen.findByRole("progressbar", {
    name: "采集中 · 日线 SHFE",
  })) as HTMLProgressElement;
  expect(bar.value).toBe(126);
  expect(bar.max).toBe(210);
  expect(screen.getByRole("button", { name: "● 数据" }).dataset.tone).toBe("ok");
  const services = view.container.querySelector(".status-services")!;
  expect([...services.children].map(item => item.textContent)).toEqual([
    "● 行情 连接中",
    "● 研究",
    "● 数据",
    "● 交易·simnow 连接中",
  ]);
  const button = bar.closest("button")!;
  expect(services.nextElementSibling).toBe(button);
  expect(button.textContent).toContain("采集中 · 日线 SHFE");
  expect(button.textContent).toBe("采集中 · 日线 SHFE60%");
  expect(button.title).toContain("126 / 210 个分区");
  expect(button.title).toContain("另有 1 个任务等待");
  expect(button.title).toContain("SHFE");
  expect(button.title).toContain("磁盘可用 80 GB");
  fireEvent.click(button);
  expect(openDataset).toHaveBeenCalled();

  vi.mocked(warehouseData).mockImplementation(() => new Promise(() => {}));
});

test("clearing a service while a background poll is in flight leaves notifications quiet", async () => {
  let rejectStatus!: (reason: unknown) => void;
  vi.mocked(warehouseData).mockImplementationOnce(
    () =>
      new Promise((_, reject) => {
        rejectStatus = reject;
      }),
  );
  const snapshot = readySnapshot();
  const view = render(
    <>
      {show(snapshot)}
      <NotificationCenter />
    </>,
  );
  await act(async () => {
    rejectStatus(
      new BackendError(
        "conflict",
        "service selection changed during operation; inspect the original service",
      ),
    );
  });
  view.rerender(
    <>
      {show({ ...snapshot, warehouse_endpoint: null })}
      <NotificationCenter />
    </>,
  );
  expect(screen.getByRole("button", { name: "● 数据 未设置" })).toBeTruthy();
  expect(screen.getByRole("button", { expanded: false }).className).not.toContain(
    "notification-latest",
  );
  vi.mocked(warehouseData).mockImplementation(() => new Promise(() => {}));
});

test("research and trading status indicators navigate directly to their targets", () => {
  const snapshot = readySnapshot();
  snapshot.trading_connections = [
    { id: "sim", host: "h", port: 7460, instance: "simnow", fingerprint: "f", revision: "r" },
  ];
  const openResearch = vi.fn();
  const openTrading = vi.fn();
  render(
    <WarehouseProvider endpoint="">
      <StatusIndicators
        snapshot={snapshot}
        openMarketSettings={vi.fn()}
        openMarket={vi.fn()}
        openResearch={openResearch}
        openWarehouseSettings={vi.fn()}
        openTradingSettings={vi.fn()}
        openTrading={openTrading}
      />
    </WarehouseProvider>,
  );
  fireEvent.click(screen.getByRole("button", { name: "● 研究" }));
  expect(openResearch).toHaveBeenCalledTimes(1);
  fireEvent.click(screen.getByRole("button", { name: "● 交易·simnow 连接中" }));
  expect(openTrading).toHaveBeenCalledWith("sim");
});

test("trading status indicator shows attention tone and tooltip when orders are allowed but broker is not ready", async () => {
  const snapshot = readySnapshot();
  snapshot.trading_connections = [
    { id: "sim", host: "h", port: 7460, instance: "simnow", fingerprint: "f", revision: "r" },
  ];
  vi.mocked(tradingRequest).mockImplementation(async (_id, operation) =>
    operation.case === "getAccount"
      ? create(ResponseSchema, {
          result: {
            case: "getAccount",
            value: {
              id: "rec",
              session: { brokerState: BrokerState.DISCONNECTED, authorizedMs: 1n },
            },
          },
        })
      : create(ResponseSchema, { result: { case: "getStrategies", value: {} } }),
  );
  render(
    <TradingAccountsProvider
      connections={snapshot.trading_connections.flatMap(item => ("error" in item ? [] : [item]))}
    >
      <WarehouseProvider endpoint="">
        <StatusIndicators
          snapshot={snapshot}
          openMarketSettings={vi.fn()}
          openMarket={vi.fn()}
          openWarehouseSettings={vi.fn()}
          openTradingSettings={vi.fn()}
          openTrading={vi.fn()}
        />
      </WarehouseProvider>
    </TradingAccountsProvider>,
  );
  const light = await screen.findByRole("button", { name: "● 交易·simnow 未就绪" });
  expect(light.dataset.tone).toBe("attention");
  expect(light.title).toContain("已允许下单");
});
