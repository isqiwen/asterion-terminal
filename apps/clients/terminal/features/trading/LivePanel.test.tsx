import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import type { Snapshot } from "../../src/bridge/client";
import { tradingRequest } from "../../src/bridge/trading";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import { CatalogSchema, CatalogState, type Catalog } from "../../src/generated/asterion/market_pb";
import {
  AccountSchema,
  BrokerState,
  Environment,
  InstanceState,
  ResponseSchema,
  SessionSchema,
  TradeSchema,
  type Response,
  type Session,
} from "../../src/generated/asterion/trading_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { TradingAccountsProvider } from "../../src/host/trading-accounts";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { BackendError } from "../../src/i18n/errors";
import { LivePanel } from "./LivePanel";
import { LiveStrategies } from "./LiveStrategies";
import { feature } from "./feature";

let marketCatalog: Catalog | undefined;
vi.mock("../../src/host/market", async importOriginal => {
  const actual = await importOriginal<typeof import("../../src/host/market")>();
  return { ...actual, useMarketCatalog: () => marketCatalog };
});
vi.mock("../../src/bridge/trading", () => {
  const tradingRequest = vi.fn();
  const tradingCall = async (account: string, operation: unknown) =>
    (await tradingRequest(account, operation)).result.value;
  return { tradingRequest, tradingCall };
});
registerTerminalFeatures([feature]);

const broker = { front: "tcp://127.0.0.1:1", brokerId: "9999", userId: "000001", appId: "test" };
const account = { id: "record", broker, environment: Environment.SIMULATION };
// One counter account, logged in and allowed to send orders, with nothing on it.
const session = create(SessionSchema, {
  policyRevision: "policy",
  policy: {
    limits: {
      maxOrderLots: { text: "5" },
      maxGrossLots: { text: "10" },
      maxContractLots: { text: "10" },
    },
    contracts: [
      {
        instrument: { exchange: "SHFE", symbol: "rb2610" },
        product: "rb",
        deliveryMonth: "2026-10",
        priceTick: { text: "1" },
        lotSize: { text: "1" },
        multiplier: { text: "10" },
      },
    ],
  },
  brokerState: BrokerState.READY,
  positionsReconciled: true,
  authorizedMs: 1n,
});
const live = (value: Session): Response =>
  create(ResponseSchema, {
    result: { case: "getAccount", value: create(AccountSchema, { ...account, session: value }) },
  });
const snapshot: Snapshot = {
  ...readySnapshot(),
  trading_connections: [
    {
      id: "account",
      host: "127.0.0.1",
      port: 7460,
      instance: "account-b",
      fingerprint: "a".repeat(64),
      revision: "1",
    },
  ],
};
// Trading answers every read with `read()`; commands are answered by the test.
let read = () => live(session);
let answer = (_: Response) => {};
beforeEach(() => {
  marketCatalog = undefined;
  read = () => live(session);
  vi.mocked(tradingRequest).mockReset();
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) =>
    operation?.case === "getStrategies" ||
    operation?.case === "getDay" ||
    operation?.case === "getOrders" ||
    operation?.case === "getCatalog"
      ? create(ResponseSchema, { result: { case: operation.case, value: {} } })
      : operation?.case === "getAccount" || operation?.case === "getHealth"
        ? read()
        : new Promise(resolve => {
            answer = resolve;
          }),
  );
});
const commands = () =>
  vi
    .mocked(tradingRequest)
    .mock.calls.filter(
      ([, operation]) =>
        !["getAccount", "getHealth", "getStrategies", "getDay", "getOrders", "getCatalog"].includes(
          operation.case!,
        ),
    );
const button = (name: string) => screen.getByRole("button", { name }) as HTMLButtonElement;
const show = (namespace: string) =>
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace={namespace}>
      <TradingAccountsProvider connections={[{ id: "account", revision: "1" }]}>
        <LivePanel
          snapshot={snapshot}
          busy={false}
          error=""
          navigate={() => {}}
          openSettings={() => {}}
          trade={async () => {}}
        />
      </TradingAccountsProvider>
    </WorkspaceDraftScope>,
  );

test("a revoke is sent while another request of the account is in flight", async () => {
  show("revoke");
  await screen.findByRole("button", { name: "停止下单" });
  fireEvent.click(button("断开账户"));
  // What starts or ends something else waits for the request in flight; what
  // takes the permission to send back does not.
  expect(button("断开账户").disabled).toBe(true);
  expect(button("停止下单").disabled).toBe(false);
  fireEvent.click(button("停止下单"));
  expect(commands()).toEqual([
    ["account", { case: "disconnectBroker", value: {} }],
    [
      "account",
      {
        case: "sendCommand",
        value: {
          requestId: expect.any(String),
          accountId: "record",
          policyRevision: "policy",
          action: { case: "revoke", value: {} },
        },
      },
    ],
  ]);
  // Only a second press is held back, until the first is answered.
  expect(button("停止下单").disabled).toBe(true);
  await act(async () => answer(live(session)));
  await waitFor(() => expect(button("停止下单").disabled).toBe(false));
});

test("a buy order names the account, the reviewed policy and exact decimals", async () => {
  show("order");
  fireEvent.change(await screen.findByRole("textbox", { name: "委托手数" }), {
    target: { value: "2" },
  });
  expect(screen.queryByRole("checkbox", { name: "我已核对账户与柜台环境" })).toBeNull();
  expect(screen.queryByText("CTP 环境待确认")).toBeNull();
  fireEvent.change(screen.getByRole("textbox", { name: "限价" }), {
    target: { value: "3561.5" },
  });
  fireEvent.click(button("买入 rb2610 · account-b"));
  expect(commands()).toEqual([
    [
      "account",
      {
        case: "sendCommand",
        value: {
          requestId: expect.any(String),
          accountId: "record",
          policyRevision: "policy",
          action: {
            case: "submitOrder",
            value: {
              orderId: expect.any(String),
              instrument: expect.objectContaining({ exchange: "SHFE", symbol: "rb2610" }),
              side: Side.BUY,
              offset: Offset.OPEN,
              lots: expect.objectContaining({ text: "2" }),
              price: expect.objectContaining({ text: "3561.5" }),
            },
          },
        },
      },
    ],
  ]);
});

test("a real-money order is sent only after its summary is confirmed", async () => {
  read = () =>
    create(ResponseSchema, {
      result: {
        case: "getAccount",
        value: create(AccountSchema, { ...account, environment: Environment.REAL, session }),
      },
    });
  show("real");
  // Real-money warning is visible immediately before the environment checkbox is ticked.
  const identity = (await screen.findByText("CTP 环境待确认")).closest(".ctp-identity")!;
  expect(identity.hasAttribute("data-real")).toBe(true);
  expect(identity.textContent).toContain("真实资金");
  fireEvent.click(screen.getByRole("checkbox", { name: "我已核对账户与柜台环境" }));
  fireEvent.change(await screen.findByRole("textbox", { name: "限价" }), {
    target: { value: "3561" },
  });
  fireEvent.click(button("买入 rb2610 · account-b"));
  const summary = screen.getByRole("alertdialog", { name: "确认委托" });
  expect(summary.textContent).toContain("SHFE · rb2610");
  expect(commands()).toEqual([]);
  fireEvent.click(button("取消"));
  expect(screen.queryByRole("alertdialog")).toBeNull();
  expect(commands()).toEqual([]);
  fireEvent.click(button("买入 rb2610 · account-b"));
  fireEvent.click(button("确认发送"));
  expect(commands()).toHaveLength(1);
  expect(commands()[0][1]).toMatchObject({
    case: "sendCommand",
    value: { action: { case: "submitOrder", value: { side: Side.BUY } } },
  });
});

test("an account without a policy offers to open trading in the service's environment", async () => {
  marketCatalog = create(CatalogSchema, {
    status: { state: CatalogState.READY, tradingDay: "2026-11-27" },
    contracts: [
      {
        instrument: { exchange: "SHFE", symbol: "rb2610" },
        contract: "SHFE/rb/2026-10",
        product: "rb",
        name: "螺纹钢2610",
        multiplier: 10,
        priceTick: { text: "1" },
      },
    ],
  });
  read = () =>
    create(ResponseSchema, {
      result: { case: "getAccount", value: create(AccountSchema, account) },
    });
  show("create");
  expect(await screen.findByText("开通交易：account-b")).toBeTruthy();
  expect(screen.getByRole("status", { name: "CTP 账户" }).textContent).toContain("account-b");
  expect(screen.getAllByText("CTP 仿真").length).toBeGreaterThan(0);
  expect(screen.getByText("已选 0 个合约")).toBeTruthy();
  expect(screen.getByText("已填 0/5 项必填")).toBeTruthy();
  const picker = screen.getByRole("combobox", { name: "添加合约" });
  fireEvent.focus(picker);
  fireEvent.mouseDown(await screen.findByRole("option", { name: /SHFE\.rb2610/ }));
  fireEvent.click(screen.getByRole("button", { name: "填入轻仓仿真参考" }));
  expect(screen.getByText("已选 1 个合约")).toBeTruthy();
  expect(screen.getByText("已填 5/5 项必填")).toBeTruthy();
});

test("account setup surfaces catalog sync failures with a retry button", async () => {
  let failCatalog = true;
  read = () =>
    create(ResponseSchema, {
      result: { case: "getAccount", value: create(AccountSchema, account) },
    });
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) => {
    if (operation?.case === "getCatalog") {
      if (failCatalog) throw new BackendError("permission_denied", "CTP catalog login rejected: 3");
      return create(ResponseSchema, {
        result: {
          case: "getCatalog",
          value: {
            contracts: [
              {
                instrument: { exchange: "SHFE", symbol: "rb2610" },
                product: "rb",
                name: "螺纹钢2610",
                multiplier: 10,
                priceTick: { text: "1" },
              },
            ],
          },
        },
      });
    }
    if (operation?.case === "getStrategies")
      return create(ResponseSchema, { result: { case: "getStrategies", value: {} } });
    return read();
  });
  show("create-catalog-retry");
  expect(await screen.findByText(/合约目录登录失败.*CTP 3/)).toBeTruthy();
  failCatalog = false;
  fireEvent.click(screen.getByRole("button", { name: "重试" }));
  expect(await screen.findByRole("combobox", { name: "添加合约" })).toBeTruthy();
});

test("account setup reuses cached Market contracts while Market refreshes without calling Trading getCatalog", async () => {
  marketCatalog = create(CatalogSchema, {
    status: { state: CatalogState.LOADING, tradingDay: "2026-11-27" },
    contracts: [
      {
        instrument: { exchange: "SHFE", symbol: "rb2610" },
        contract: "SHFE/rb/2026-10",
        product: "rb",
        name: "螺纹钢2610",
        multiplier: 10,
        priceTick: { text: "1" },
      },
    ],
  });
  read = () =>
    create(ResponseSchema, {
      result: { case: "getAccount", value: create(AccountSchema, account) },
    });
  show("create-cached-market-catalog");
  const picker = await screen.findByRole("combobox", { name: "添加合约" });
  fireEvent.focus(picker);
  expect(await screen.findByRole("option", { name: /SHFE\.rb2610/ })).toBeTruthy();
  expect(
    vi.mocked(tradingRequest).mock.calls.filter(([, op]) => op?.case === "getCatalog"),
  ).toHaveLength(0);
});

test("an unreachable trading service explains the failure and links to trading settings", async () => {
  vi.mocked(tradingRequest).mockRejectedValue(
    new BackendError("unavailable", "RPC connection refused"),
  );
  const openSettings = vi.fn();
  const offline = readySnapshot();
  const connections = [
    {
      id: "offline-acct",
      host: "192.168.50.20",
      port: 7460,
      instance: "simnow",
      fingerprint: "f".repeat(64),
      revision: "1",
    },
  ];
  offline.trading_connections = connections;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="offline">
      <TradingAccountsProvider connections={connections}>
        <LivePanel
          snapshot={offline}
          busy={false}
          error=""
          trade={vi.fn()}
          navigate={vi.fn()}
          openSettings={openSettings}
        />
      </TradingAccountsProvider>
    </WorkspaceDraftScope>,
  );
  const alert = await screen.findByRole("alert");
  expect(alert.textContent).toContain("服务未连接");
  expect(alert.textContent).toContain("192.168.50.20:7460");
  expect(alert.textContent).toContain("服务拒绝连接，请求未发送");
  fireEvent.click(screen.getByRole("button", { name: "交易服务设置" }));
  expect(openSettings).toHaveBeenCalledWith("trading");
});

test("the account's broker facts follow each read", async () => {
  show("facts");
  expect(await screen.findByText("持仓已核对")).toBeTruthy();
  read = () =>
    live({ ...session, brokerState: BrokerState.DISCONNECTED, positionsReconciled: false });
  expect(await screen.findByText("持仓待核对", undefined, { timeout: 3000 })).toBeTruthy();
  expect(commands()).toEqual([]);
});

test("ordinary CTP login failures stay out of page banners while callback queue overflow keeps its safety alert", async () => {
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        brokerState: BrokerState.FAILED,
        ctpError: 7,
        authorizedMs: undefined,
      } as never),
    );
  show("broker-failed-7");
  const phase = await screen.findByTestId("live-phase");
  expect(phase.textContent).toBe("连接失败");
  expect(phase.closest("[title]")?.getAttribute("title")).toContain("CTP 7");
  expect(screen.queryByRole("alert")).toBeNull();

  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        brokerState: BrokerState.FAILED,
        ctpError: -1008,
        authorizedMs: undefined,
      } as never),
    );
  expect(
    await screen.findByText("柜台回报队列已满，已停止报单；重连后核对账户", undefined, {
      timeout: 3000,
    }),
  ).toBeTruthy();
});

test("trading service disconnect stays on the status badge while ledger recovery mode keeps an inline safety alert", async () => {
  let unreachable = false;
  read = () => {
    if (unreachable) throw new BackendError("unavailable", "RPC connection refused");
    return live(session);
  };
  show("stale-vs-recovery");
  expect((await screen.findByTestId("live-phase")).textContent).toBe("已就绪");
  unreachable = true;
  await waitFor(() => expect(screen.getByTestId("live-phase").textContent).toBe("服务失联"), {
    timeout: 3000,
  });
  expect(screen.getByTestId("live-phase").closest("[title]")?.getAttribute("title")).toBe(
    "交易服务失联，显示的是最后读到的状态",
  );
  expect(screen.queryByRole("alert")).toBeNull();

  unreachable = false;
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        recoveryRequired: true,
      } as never),
    );
  await waitFor(() => expect(screen.getByTestId("live-phase").textContent).toBe("需恢复"), {
    timeout: 3000,
  });
  expect(screen.getByRole("alert").textContent).toContain("恢复模式");
});

test("an earlier trading day shows its recorded close and fills and exports them", async () => {
  const recorded = create(TradeSchema, {
    id: "SHFE:     1",
    instrument: { exchange: "SHFE", symbol: "rb2610" },
    side: Side.BUY,
    offset: Offset.OPEN,
    lots: { text: "1" },
    price: { text: "3500" },
    tradingDay: "2026-09-25",
    tradeTime: "20260925 09:30:00",
  });
  read = () => live({ ...session, tradingDay: "2026-09-28" });
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) =>
    operation?.case === "getDay"
      ? create(ResponseSchema, {
          result: {
            case: "getDay",
            value: {
              tradingDays: ["2026-09-25", "2026-09-28"],
              trades: operation.value.tradingDay === "2026-09-25" ? [recorded] : [],
              close:
                operation.value.tradingDay === "2026-09-25"
                  ? {
                      tradingDay: "2026-09-25",
                      funds: { balance: { text: "1000123.5" }, available: { text: "990000" } },
                      positions: [
                        {
                          instrument: { exchange: "SHFE", symbol: "rb2610" },
                          side: Side.BUY,
                          todayLots: { text: "1" },
                          yesterdayLots: { text: "0" },
                        },
                      ],
                    }
                  : undefined,
            },
          },
        })
      : read(),
  );
  show("fills");
  fireEvent.click(await screen.findByRole("button", { name: "成交" }));
  const day = await screen.findByRole("combobox", { name: "交易日" });
  // Days are read through the Trading connection, not the counter's account name.
  expect(
    vi
      .mocked(tradingRequest)
      .mock.calls.filter(([, operation]) => operation.case === "getDay")
      .every(([connection]) => connection === "account"),
  ).toBe(true);
  expect([...(day as HTMLSelectElement).options].map(option => option.value)).toEqual([
    "",
    "2026-09-25",
  ]);
  expect(screen.getByText("暂无成交")).toBeTruthy();
  fireEvent.change(day, { target: { value: "2026-09-25" } });
  expect(await screen.findByText("20260925 09:30:00")).toBeTruthy();
  expect(screen.queryByText("暂无成交")).toBeNull();
  expect(screen.getByText("1,000,123.5")).toBeTruthy();
  expect(screen.getByRole("table", { name: "收盘持仓" }).textContent).toContain("rb2610");
  // The export is what the page shows.
  let exported: Blob | undefined;
  URL.createObjectURL = vi.fn((blob: Blob) => {
    exported = blob;
    return "blob:day";
  });
  URL.revokeObjectURL = vi.fn();
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
  fireEvent.click(screen.getByRole("button", { name: "导出 CSV" }));
  await waitFor(() => expect(click).toHaveBeenCalled());
  const text = await exported!.text();
  expect(text).toContain("交易日,2026-09-25");
  expect(text).toContain('"1,000,123.5","990,000"');
  expect(text).toContain("rb2610,多头,1,0");
  expect(text).toContain("rb2610,买入 / 开仓,20260925 09:30:00,3500,1");
  click.mockRestore();
});

test("a position shows the broker's amounts and its close fills the ticket without sending", async () => {
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        positions: [
          {
            instrument: { exchange: "SHFE", symbol: "rb2610" },
            side: Side.BUY,
            todayLots: { text: "2" },
            yesterdayLots: { text: "1" },
            cost: { text: "105000" },
            profit: { text: "300" },
            margin: { text: "10500" },
          },
        ],
        // A working sell close already takes one lot.
        workingOrders: [
          {
            id: "closing",
            instrument: { exchange: "SHFE", symbol: "rb2610" },
            side: Side.SELL,
            offset: Offset.CLOSE_YESTERDAY,
            lots: { text: "1" },
            filledLots: { text: "0" },
            state: 2,
          },
        ],
      } as never),
    );
  show("positions");
  const row = (await screen.findByRole("cell", { name: "3500.00" })).closest("tr")!;
  // Three lots, one taken by the working close; 105,000 / (3 × 10).
  expect(row.textContent).toContain("2");
  expect(row.textContent).toContain("10,500");
  const sent = commands().length;
  fireEvent.click(screen.getByRole("button", { name: "平仓" }));
  // SHFE closes today's lots on their own.
  await waitFor(() =>
    expect((screen.getByRole("textbox", { name: "委托手数" }) as HTMLInputElement).value).toBe("2"),
  );
  expect(
    within(screen.getByRole("group", { name: "开平仓" }))
      .getByRole("button", { name: "平今" })
      .getAttribute("aria-pressed"),
  ).toBe("true");
  expect(button("卖出 rb2610 · account-b").hasAttribute("data-suggested")).toBe(true);
  expect(commands()).toHaveLength(sent);
});

test("an explicit-bucket position closes yesterday's remaining lots when today's are held", async () => {
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        positions: [
          {
            instrument: { exchange: "SHFE", symbol: "rb2610" },
            side: Side.BUY,
            todayLots: { text: "2" },
            yesterdayLots: { text: "1" },
            cost: { text: "105000" },
            profit: { text: "300" },
            margin: { text: "10500" },
          },
        ],
        workingOrders: [
          {
            id: "closing-today",
            instrument: { exchange: "SHFE", symbol: "rb2610" },
            side: Side.SELL,
            offset: Offset.CLOSE_TODAY,
            lots: { text: "2" },
            filledLots: { text: "0" },
            state: 2,
          },
        ],
      } as never),
    );
  show("positions-yesterday");
  await screen.findByRole("cell", { name: "3500.00" });
  fireEvent.click(screen.getByRole("button", { name: "平仓" }));
  await waitFor(() =>
    expect((screen.getByRole("textbox", { name: "委托手数" }) as HTMLInputElement).value).toBe("1"),
  );
  expect(
    within(screen.getByRole("group", { name: "开平仓" }))
      .getByRole("button", { name: "平昨" })
      .getAttribute("aria-pressed"),
  ).toBe("true");
});

test("the strategy view shares the selected account and marks real-money accounts", async () => {
  read = () =>
    create(ResponseSchema, {
      result: {
        case: "getAccount",
        value: create(AccountSchema, { ...account, environment: Environment.REAL, session }),
      },
    });
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace={feature.id}>
      <TradingAccountsProvider connections={[{ id: "account", revision: "1" }]}>
        <LiveStrategies
          snapshot={snapshot}
          busy={false}
          error=""
          navigate={() => {}}
          openSettings={() => {}}
          trade={async () => {}}
        />
      </TradingAccountsProvider>
    </WorkspaceDraftScope>,
  );
  expect(await screen.findByText("真实资金")).toBeTruthy();
});

test("every working order is cancelled only after a second press", async () => {
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        workingOrders: ["a", "b"].map(id => ({
          id,
          brokerKey: id,
          instrument: { exchange: "SHFE", symbol: "rb2610" },
          side: Side.BUY,
          offset: Offset.OPEN,
          lots: { text: "1" },
          filledLots: { text: "0" },
          state: 2,
        })),
      } as never),
    );
  show("cancel-all");
  fireEvent.click(await screen.findByRole("button", { name: "委托" }));
  fireEvent.click(await screen.findByRole("button", { name: "全部撤单" }));
  expect(commands()).toHaveLength(0);
  fireEvent.click(screen.getByRole("button", { name: "确认撤掉 2 笔" }));
  await waitFor(() => expect(commands()).toHaveLength(1));
  await act(async () => answer(live(session)));
  await waitFor(() => expect(commands()).toHaveLength(2));
  expect(
    commands().map(
      ([, operation]) =>
        (operation.value as { action: { case: string; value: { id: string } } }).action.value.id,
    ),
  ).toEqual(["a", "b"]);
});

test("a close order above maxOrderLots splits into maxOrderLots legs", async () => {
  show("split-close");
  fireEvent.click(
    within(await screen.findByRole("group", { name: "开平仓" })).getByRole("button", {
      name: "平今",
    }),
  );
  fireEvent.change(screen.getByRole("textbox", { name: "委托手数" }), {
    target: { value: "12" },
  });
  fireEvent.change(screen.getByRole("textbox", { name: "限价" }), {
    target: { value: "3560" },
  });
  fireEvent.click(button("卖出 rb2610 · account-b"));
  await waitFor(() => expect(commands()).toHaveLength(1));
  await act(async () => answer(live(session)));
  await waitFor(() => expect(commands()).toHaveLength(2));
  await act(async () => answer(live(session)));
  await waitFor(() => expect(commands()).toHaveLength(3));
  await act(async () => answer(live(session)));
  expect(
    commands().map(
      ([, operation]) =>
        (
          operation.value as {
            action: { case: string; value: { lots: { text: string }; offset: Offset } };
          }
        ).action.value,
    ),
  ).toEqual([
    expect.objectContaining({
      lots: expect.objectContaining({ text: "5" }),
      offset: Offset.CLOSE_TODAY,
    }),
    expect.objectContaining({
      lots: expect.objectContaining({ text: "5" }),
      offset: Offset.CLOSE_TODAY,
    }),
    expect.objectContaining({
      lots: expect.objectContaining({ text: "2" }),
      offset: Offset.CLOSE_TODAY,
    }),
  ]);
});

test("a split close keeps only remaining lots when a middle leg fails, and switching contract clears limit price", async () => {
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        policy: {
          ...session.policy,
          contracts: [
            ...session.policy!.contracts,
            {
              instrument: { exchange: "DCE", symbol: "m2609" },
              product: "m",
              deliveryMonth: "2026-09",
              priceTick: { text: "1" },
              lotSize: { text: "1" },
              multiplier: { text: "10" },
            },
          ],
        },
        positions: [
          {
            instrument: { exchange: "DCE", symbol: "m2609" },
            side: Side.BUY,
            todayLots: { text: "3" },
            yesterdayLots: { text: "0" },
            cost: { text: "90000" },
            profit: { text: "0" },
            margin: { text: "9000" },
          },
        ],
      } as never),
    );
  let callCount = 0;
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) => {
    if (
      operation?.case === "getStrategies" ||
      operation?.case === "getDay" ||
      operation?.case === "getOrders"
    )
      return create(ResponseSchema, { result: { case: operation.case, value: {} } });
    if (operation?.case === "getAccount" || operation?.case === "getHealth") return read();
    callCount += 1;
    if (callCount === 2) throw new Error("rejected second leg");
    return read();
  });
  show("split-partial");
  fireEvent.click(
    within(await screen.findByRole("group", { name: "开平仓" })).getByRole("button", {
      name: "平今",
    }),
  );
  fireEvent.change(screen.getByRole("textbox", { name: "委托手数" }), {
    target: { value: "12" },
  });
  fireEvent.change(screen.getByRole("textbox", { name: "限价" }), {
    target: { value: "3560" },
  });
  fireEvent.click(button("卖出 rb2610 · account-b"));
  await waitFor(() =>
    expect((screen.getByRole("textbox", { name: "委托手数" }) as HTMLInputElement).value).toBe("7"),
  );
  // Closing an unquoted second contract clears the previous contract's limit price.
  fireEvent.click(screen.getByRole("button", { name: "平仓" }));
  await waitFor(() =>
    expect((screen.getByRole("textbox", { name: "限价" }) as HTMLInputElement).value).toBe(""),
  );
});

test("editing risk limits resets policy confirmation so Enter cannot submit stale approval", async () => {
  show("policy-reset");
  fireEvent.click(await screen.findByRole("button", { name: "风控" }));
  const confirmBox = (await screen.findByRole("checkbox", {
    name: "我确认应用上述风控设置并重新登录",
  })) as HTMLInputElement;
  fireEvent.click(confirmBox);
  expect(confirmBox.checked).toBe(true);
  fireEvent.change(screen.getByRole("textbox", { name: "单笔数量上限" }), {
    target: { value: "6" },
  });
  expect(confirmBox.checked).toBe(false);
  expect(button("应用新风控设置").disabled).toBe(true);
  expect(screen.getByText(/^当前 /)).toBeTruthy();
});

test("policy editor loads selectable contracts from Trading when Market is offline", async () => {
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) => {
    if (operation?.case === "getCatalog")
      return create(ResponseSchema, {
        result: {
          case: "getCatalog",
          value: {
            tradingDay: "2026-09-28",
            contracts: [
              {
                instrument: { exchange: "DCE", symbol: "m2701" },
                contract: "DCE/m/2027-01",
                product: "m",
                name: "豆粕2701",
                expiryDay: "2027-01-15",
                multiplier: 10,
                priceTick: { text: "1" },
              },
            ],
          },
        },
      });
    if (
      operation?.case === "getStrategies" ||
      operation?.case === "getDay" ||
      operation?.case === "getOrders"
    )
      return create(ResponseSchema, { result: { case: operation.case, value: {} } });
    return read();
  });
  show("policy-offline-market");
  fireEvent.click(await screen.findByRole("button", { name: "风控" }));
  const picker = await screen.findByRole("combobox", { name: "新增可交易合约" });
  await waitFor(() => expect(screen.queryByText("合约目录未就绪，暂不能新增合约")).toBeNull());
  fireEvent.focus(picker);
  fireEvent.change(picker, { target: { value: "豆粕" } });
  fireEvent.mouseDown(await screen.findByRole("option", { name: /DCE\.m2701/ }));
  expect(await screen.findByText(/m2701/)).toBeTruthy();
});

test("hard risk breaches show an alert banner and policy editor submits capital and cancel limits in percent", async () => {
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        policy: {
          ...session.policy,
          limits: {
            maxOrderLots: { text: "5" },
            maxGrossLots: { text: "10" },
            maxContractLots: { text: "10" },
            maxWorkingOrders: 4,
          },
          maxPriceDeviation: { text: "0.02" },
        },
        riskBreaches: ["daily_loss", "daily_cancels:SHFE.rb2610"],
      } as never),
    );
  show("capital-risk");
  expect(
    await screen.findByText(/硬风控已触发，暂停新增开仓：日内亏损达限、SHFE\.rb2610 日撤单达限/),
  ).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "风控" }));
  fireEvent.change(await screen.findByRole("textbox", { name: "日内亏损限额" }), {
    target: { value: "15000" },
  });
  fireEvent.change(screen.getByRole("textbox", { name: "最大回撤限额" }), {
    target: { value: "30000" },
  });
  // Ratios are percentages in the form: 2 is the recorded 0.02, 70 is 0.7.
  expect(screen.getByRole<HTMLInputElement>("textbox", { name: "价格偏离上限" }).value).toBe("2");
  fireEvent.change(screen.getByRole("textbox", { name: "保证金率上限" }), {
    target: { value: "70" },
  });
  fireEvent.change(screen.getByRole("textbox", { name: "单合约日撤单上限" }), {
    target: { value: "300" },
  });
  fireEvent.click(screen.getByRole("checkbox", { name: "我确认应用上述风控设置并重新登录" }));
  fireEvent.click(button("应用新风控设置"));
  await waitFor(() => expect(commands()).toHaveLength(1));
  expect(commands()[0]?.[1]).toEqual({
    case: "sendCommand",
    value: expect.objectContaining({
      action: {
        case: "changePolicy",
        value: expect.objectContaining({
          maxDailyLoss: expect.objectContaining({ text: "15000" }),
          maxDrawdown: expect.objectContaining({ text: "30000" }),
          maxPriceDeviation: expect.objectContaining({ text: "0.02" }),
          maxMarginRatio: expect.objectContaining({ text: "0.7" }),
          maxDailyCancelsPerContract: 300,
        }),
      },
    }),
  });
});

test("protocol mismatch disables routine trading while keeping stop strategy, cancel order, and revoke live", async () => {
  let authorized = true;
  let activeStrategyId = "inst-emergency";
  let orders = [
    {
      id: "ord-emg-1",
      exchange: "SHFE",
      symbol: "rb2610",
      side: "buy",
      offset: "open",
      lots: "3",
      price: "3500",
    },
  ];
  const emergencyState = () => ({
    protocol: "emg-v1",
    accountId: "record",
    policyRevision: "policy",
    accountOpen: true,
    brokerReady: true,
    authorized,
    activeStrategyId,
    workingOrders: orders,
  });
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) => {
    if (operation?.case === "getEmergency")
      return create(ResponseSchema, {
        result: { case: "getEmergency", value: emergencyState() },
      });
    if (operation?.case === "emergencyControl") {
      const action = operation.value.action;
      if (action?.case === "stopStrategy" && action.value.instanceId === activeStrategyId)
        activeStrategyId = "";
      if (action?.case === "cancelOrder")
        orders = orders.filter(item => item.id !== action.value.orderId);
      if (action?.case === "revoke") authorized = false;
      return create(ResponseSchema, {
        result: { case: "emergencyControl", value: emergencyState() },
      });
    }
    throw new BackendError("conflict", "trading protocol differs from this Terminal");
  });

  show("emergency-mismatch");
  expect(
    await screen.findByText(/交易服务与 Terminal 的协议不一致，常规业务已禁用；仍可执行紧急制动。/),
  ).toBeTruthy();
  expect(screen.queryByRole("textbox", { name: "委托手数" })).toBeNull();

  fireEvent.click(button("停止策略并撤单"));
  await waitFor(() => expect(screen.queryByRole("button", { name: "停止策略并撤单" })).toBeNull());

  fireEvent.click(button("撤单"));
  await waitFor(() => expect(screen.queryByText("SHFE.rb2610")).toBeNull());

  fireEvent.click(button("停止下单"));
  await waitFor(() => expect(screen.queryByRole("button", { name: "停止下单" })).toBeNull());
});

test("policy editor explains when broker is disconnected and strategy view consumes account param once", async () => {
  read = () =>
    live(
      create(SessionSchema, {
        ...(session as object),
        brokerState: BrokerState.DISCONNECTED,
      } as never),
    );
  show("policy-disconnected");
  fireEvent.click(await screen.findByRole("button", { name: "风控" }));
  expect(await screen.findByText("账户连接并同步后可修改风控设置")).toBeTruthy();

  const twoAccounts: Snapshot = {
    ...readySnapshot(),
    trading_connections: [
      {
        id: "account-1",
        host: "127.0.0.1",
        port: 7460,
        instance: "sim-one",
        fingerprint: "a".repeat(64),
        revision: "1",
      },
      {
        id: "account-2",
        host: "127.0.0.1",
        port: 7461,
        instance: "sim-two",
        fingerprint: "b".repeat(64),
        revision: "1",
      },
    ],
  };
  const navigate = vi.fn();
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="strategy-account-param">
      <TradingAccountsProvider
        connections={(twoAccounts.trading_connections ?? []).flatMap(item =>
          "error" in item ? [] : [item],
        )}
      >
        <LiveStrategies
          snapshot={twoAccounts}
          busy={false}
          error=""
          workspaceParams={{ account: "account-2" }}
          navigate={navigate}
          openSettings={() => {}}
          trade={async () => {}}
        />
      </TradingAccountsProvider>
    </WorkspaceDraftScope>,
  );
  await waitFor(() =>
    expect(screen.getByRole("button", { name: /^sim-two/ }).getAttribute("aria-current")).toBe(
      "true",
    ),
  );
  expect(navigate).toHaveBeenCalledWith("workspace.trading-strategies", {});
});

test("a running strategy replaces the manual ticket in the workbench rail and supports direct pause", async () => {
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) => {
    if (operation?.case === "getStrategies")
      return create(ResponseSchema, {
        result: {
          case: "getStrategies",
          value: {
            instance: {
              instance: {
                id: "soak-1",
                packageName: "ma_cross.MaCross",
                instruments: [{ exchange: "SHFE", symbol: "rb2610" }],
              },
              state: InstanceState.READY,
              completedSteps: 3n,
            },
          },
        },
      });
    if (operation?.case === "pauseStrategy")
      return create(ResponseSchema, {
        result: {
          case: "pauseStrategy",
          value: {
            instance: {
              instance: {
                id: "soak-1",
                packageName: "ma_cross.MaCross",
                instruments: [{ exchange: "SHFE", symbol: "rb2610" }],
              },
              state: InstanceState.READY,
              paused: true,
              completedSteps: 3n,
            },
          },
        },
      });
    if (
      operation?.case === "getDay" ||
      operation?.case === "getOrders" ||
      operation?.case === "getCatalog"
    )
      return create(ResponseSchema, { result: { case: operation.case, value: {} } });
    return read();
  });
  show("strategy-rail");
  expect(await screen.findByText("策略已就绪")).toBeTruthy();
  expect(screen.queryByRole("form", { name: "CTP 委托" })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "暂停新增" }));
  expect(await screen.findByRole("button", { name: "恢复新增" })).toBeTruthy();
});

test("the strategy view exposes authorization state and emergency stop button", async () => {
  read = () => live({ ...session, authorizedMs: undefined });
  const navigate = vi.fn();
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="strategy-auth-bar">
      <TradingAccountsProvider connections={[{ id: "account", revision: "1" }]}>
        <LiveStrategies
          snapshot={snapshot}
          busy={false}
          error=""
          navigate={navigate}
          openSettings={() => {}}
          trade={async () => {}}
        />
      </TradingAccountsProvider>
    </WorkspaceDraftScope>,
  );
  expect(await screen.findByRole("status", { name: "下单状态" })).toBeTruthy();
  fireEvent.click(await screen.findByRole("button", { name: "前往允许下单" }));
  expect(navigate).toHaveBeenCalledWith("workspace.trading", { params: { account: "account" } });
});

test("the strategy view shows a centered status card with settings entry when trading is unreachable", async () => {
  vi.mocked(tradingRequest).mockRejectedValue(
    new BackendError("unavailable", "connection refused"),
  );
  const openSettings = vi.fn();
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="strategy-unreachable">
      <TradingAccountsProvider connections={[{ id: "account", revision: "1" }]}>
        <LiveStrategies
          snapshot={snapshot}
          busy={false}
          error=""
          navigate={() => {}}
          openSettings={openSettings}
          trade={async () => {}}
        />
      </TradingAccountsProvider>
    </WorkspaceDraftScope>,
  );
  const alert = await screen.findByRole("alert");
  expect(within(alert).getByText("account-b")).toBeTruthy();
  expect(within(alert).getByText("服务未连接")).toBeTruthy();
  expect(within(alert).getByText("127.0.0.1:7460")).toBeTruthy();
  expect(within(alert).queryByRole("button", { name: "重试" })).toBeNull();
  expect(screen.queryByRole("region", { name: "策略版本" })).toBeNull();
  fireEvent.click(within(alert).getByRole("button", { name: "交易服务设置" }));
  expect(openSettings).toHaveBeenCalledWith("trading");
});

test("the strategy view shows deployment guidance in the main pane when no packages exist", async () => {
  read = () => live(session);
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="strategy-empty-packages">
      <TradingAccountsProvider connections={[{ id: "account", revision: "1" }]}>
        <LiveStrategies
          snapshot={snapshot}
          busy={false}
          error=""
          navigate={() => {}}
          openSettings={() => {}}
          trade={async () => {}}
        />
      </TradingAccountsProvider>
    </WorkspaceDraftScope>,
  );
  expect(await screen.findByText("尚未部署策略版本")).toBeTruthy();
  expect(screen.getByRole("region", { name: "策略版本" })).toBeTruthy();
});
