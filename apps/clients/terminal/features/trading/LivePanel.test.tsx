import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import type { Snapshot } from "../../src/bridge/client";
import { tradingRequest } from "../../src/bridge/trading";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import {
  AccountSchema,
  BrokerState,
  Environment,
  ResponseSchema,
  SessionSchema,
  type Response,
  type Session,
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
const account = { id: "record", broker, environment: Environment.SIMULATION };
// One counter account, logged in and allowed to send orders, with nothing on it.
const session = create(SessionSchema, {
  policyRevision: "policy",
  policy: {
    limits: { maxOrderLots: { text: "5" }, maxGrossLots: { text: "10" } },
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
  read = () => live(session);
  vi.mocked(tradingRequest).mockReset();
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) =>
    operation?.case === "getAccount" || operation?.case === "getHealth"
      ? read()
      : new Promise(resolve => {
          answer = resolve;
        }),
  );
});
const commands = () =>
  vi
    .mocked(tradingRequest)
    .mock.calls.filter(([, operation]) => !["getAccount", "getHealth"].includes(operation.case!));
const button = (name: string) => screen.getByRole("button", { name }) as HTMLButtonElement;
const show = (namespace: string) =>
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace={namespace}>
      <TradingAccountsProvider ids={["account"]} active protocol="">
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
  fireEvent.click(button("买入 account-b"));
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

test("an account without a policy offers to open trading in the service's environment", async () => {
  read = () =>
    create(ResponseSchema, {
      result: { case: "getAccount", value: create(AccountSchema, account) },
    });
  show("create");
  expect(await screen.findByText("开通交易：account-b")).toBeTruthy();
  expect(screen.getAllByText("CTP 仿真").length).toBeGreaterThan(0);
});

test("the account's broker facts follow each read", async () => {
  show("facts");
  expect(await screen.findByText("已纳入已知成交")).toBeTruthy();
  read = () =>
    live({ ...session, brokerState: BrokerState.DISCONNECTED, positionsReconciled: false });
  expect(await screen.findByText("尚未核对已知成交", undefined, { timeout: 3000 })).toBeTruthy();
  expect(commands()).toEqual([]);
});
