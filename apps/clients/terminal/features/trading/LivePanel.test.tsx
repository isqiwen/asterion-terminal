import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import type { Snapshot } from "../../src/bridge/client";
import { tradingRequest } from "../../src/bridge/trading";
import {
  LiveSnapshotSchema,
  Offset,
  ResponseSchema,
  Side,
  TradingEnvironment,
  type LiveSnapshot,
  type Response,
} from "../../src/generated/asterion/trading_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { TradingAccountsProvider } from "../../src/host/trading-accounts";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { LivePanel } from "./LivePanel";
import { feature } from "./feature";

vi.mock("../../src/bridge/trading", () => ({ tradingRequest: vi.fn() }));
registerTerminalFeatures([feature]);

const broker = { front: "tcp://127.0.0.1:1", brokerId: "9999", userId: "000001", appId: "test" };
const account = {
  accountId: "record",
  broker,
  environment: TradingEnvironment.BROKER_SIMULATION,
};
// One counter account, logged in and allowed to send orders, with nothing on it.
const session = create(LiveSnapshotSchema, {
  accountId: "record",
  policyRevision: "policy",
  environment: TradingEnvironment.BROKER_SIMULATION,
  broker,
  risk: { maxOrderQuantity: { units: 500000000n }, maxGrossQuantity: { units: 1000000000n } },
  contracts: [
    {
      venue: "SHFE",
      symbol: "rb2610",
      product: "rb",
      deliveryMonth: "2026-10",
      currency: "CNY",
      priceIncrement: { units: 100000000n },
      quantityIncrement: { units: 100000000n },
      multiplier: { units: 1000000000n },
    },
  ],
  phase: "ready",
  brokerSequence: 9007199254740993n,
  connectionGeneration: 9007199254740995n,
  positionsReconciled: true,
  authorization: { authorizedAtMs: 1n },
});
const live = (value: LiveSnapshot): Response =>
  create(ResponseSchema, { account, result: { case: "live", value } });
const snapshot: Snapshot = {
  ...readySnapshot(),
  trading_connections: [
    {
      id: "account",
      name: "测试账户",
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
  read = () => live(session);
  vi.mocked(tradingRequest).mockReset();
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) =>
    operation?.case === "attach" || operation?.case === "heartbeat"
      ? read()
      : new Promise(resolve => {
          answer = resolve;
        }),
  );
});
const commands = () =>
  vi
    .mocked(tradingRequest)
    .mock.calls.filter(([, operation]) => !["attach", "heartbeat"].includes(operation!.case!));
const button = (name: string) => screen.getByRole("button", { name }) as HTMLButtonElement;
const show = (namespace: string) =>
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace={namespace}>
      <TradingAccountsProvider ids={["account"]} active>
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
  await screen.findByRole("button", { name: "撤销授权" });
  fireEvent.click(button("断开账户"));
  // What starts or ends something else waits for the request in flight; what
  // takes the permission to send back does not.
  expect(button("断开账户").disabled).toBe(true);
  expect(button("撤销授权").disabled).toBe(false);
  fireEvent.click(button("撤销授权"));
  expect(commands().map(([id, operation, fields]) => [id, operation, fields])).toEqual([
    ["account", { case: "liveDisconnect", value: {} }, undefined],
    [
      "account",
      {
        case: "command",
        value: { requestId: expect.any(String), operation: { case: "liveRevoke", value: {} } },
      },
      { accountId: "record", policyRevision: "policy" },
    ],
  ]);
  // Only a second press is held back, until the first is answered.
  expect(button("撤销授权").disabled).toBe(true);
  await act(async () => answer(live(session)));
  await waitFor(() => expect(button("撤销授权").disabled).toBe(false));
});

test("a buy order names the account, the reviewed policy and exact decimals", async () => {
  show("order");
  fireEvent.click(await screen.findByRole("checkbox", { name: "我已核对账户与柜台环境" }));
  fireEvent.change(await screen.findByRole("textbox", { name: "委托手数" }), {
    target: { value: "2" },
  });
  fireEvent.change(screen.getByRole("textbox", { name: "限价" }), {
    target: { value: "3561.5" },
  });
  fireEvent.click(button("买入 测试账户"));
  expect(commands()).toEqual([
    [
      "account",
      {
        case: "command",
        value: {
          requestId: expect.any(String),
          operation: {
            case: "submit",
            value: {
              orderId: expect.any(String),
              venue: "SHFE",
              symbol: "rb2610",
              side: Side.BUY,
              offset: Offset.OPEN,
              quantity: expect.objectContaining({ units: 200000000n }),
              price: expect.objectContaining({ units: 356150000000n }),
            },
          },
        },
      },
      { accountId: "record", policyRevision: "policy" },
    ],
  ]);
});

test("an account without a policy offers to open trading in the service's environment", async () => {
  read = () => create(ResponseSchema, { account, result: { case: "uninitialized", value: {} } });
  show("create");
  expect(await screen.findByText("开通交易：测试账户")).toBeTruthy();
  expect(screen.getAllByText("CTP 仿真").length).toBeGreaterThan(0);
});

test("broker observation identity stays exact and follows each read", async () => {
  show("facts");
  expect(await screen.findByText("9007199254740993")).toBeTruthy();
  expect(screen.getByText("9007199254740995")).toBeTruthy();
  expect(screen.getByText("已纳入已知成交")).toBeTruthy();
  read = () => live({ ...session, phase: "disconnected", positionsReconciled: false });
  expect(await screen.findByText("尚未核对已知成交", undefined, { timeout: 3000 })).toBeTruthy();
  expect(commands()).toEqual([]);
});
