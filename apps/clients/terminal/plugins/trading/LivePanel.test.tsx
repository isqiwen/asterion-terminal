import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test } from "vitest";
import type { LiveSession, Snapshot } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { LivePanel } from "./LivePanel";
import { plugin } from "./plugin";

registerTerminalPlugins([plugin]);

// One counter account, logged in and allowed to send orders, with nothing on it.
const session: LiveSession = {
  mode: "live",
  environment: "simulation",
  account_id: "record",
  policy_revision: "policy",
  risk_artifact: "a".repeat(64),
  segment_count: 1,
  capacity: { records_used: 0, records_limit: 100001, bytes_used: 0, bytes_limit: 268435456 },
  broker: { front: "tcp://127.0.0.1:1", broker_id: "9999", user_id: "000001", app_id: "test" },
  risk: { max_order_quantity: "5", max_gross_quantity: "10", max_working_orders: 3 },
  max_price_deviation: "0.02",
  contracts: [
    {
      venue: "SHFE",
      symbol: "rb2610",
      product: "rb",
      delivery_month: "2026-10",
      currency: "CNY",
      price_increment: "1",
      quantity_increment: "1",
      multiplier: "10",
    },
  ],
  phase: "ready",
  error_code: 0,
  trading_day: "20261013",
  synchronized_ms: 1,
  broker_sequence: "9007199254740993",
  connection_generation: "9007199254740995",
  positions_reconciled: true,
  funds: null,
  positions: [],
  orders: [],
  trades: [],
  authorization: { authorized_at_ms: 1 },
  strategy: null,
  unconfirmed: [],
  costs: [],
  storage_state: "ready",
};
const snapshot: Snapshot = {
  ...readySnapshot(),
  ctp_connections: [
    {
      id: "account",
      name: "测试账户",
      broker_id: "9999",
      user_id: "000001",
      app_id: "test",
      trade_front: "tcp://127.0.0.1:1",
      market_front: "tcp://127.0.0.1:2",
      revision: "1",
      trading_record: true,
    },
  ],
  live: {
    account: {
      session,
      connection: {
        transport: "local",
        state: "connected",
        session: "live-account",
        reconnects: 0,
        restarts: 0,
        last_heartbeat_ms: 1,
        latency_ms: 1,
        health: null,
      },
    },
  },
};
const button = (name: string) => screen.getByRole("button", { name }) as HTMLButtonElement;

test("a revoke is sent while another command of the window is in flight", async () => {
  const sent: [string, Record<string, unknown> | undefined][] = [];
  let answer = () => {};
  const trade: TerminalContext["trade"] = (...[method, params]) => {
    sent.push([method, params]);
    return new Promise(resolve => {
      answer = resolve;
    });
  };
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="test">
      <LivePanel
        snapshot={snapshot}
        busy
        error=""
        query={async () => snapshot}
        navigate={() => {}}
        openSettings={() => {}}
        trade={trade}
      />
    </WorkspaceDraftScope>,
  );
  // What starts or ends something else waits for the command in flight; what
  // takes the permission to send back does not.
  expect(button("断开账户").disabled).toBe(true);
  expect(button("撤销授权").disabled).toBe(false);
  fireEvent.click(button("撤销授权"));
  expect(sent).toEqual([
    [
      "live.act",
      {
        account: "account",
        request_id: expect.any(String),
        account_id: "record",
        policy_revision: "policy",
        action: "live_revoke",
      },
    ],
  ]);
  // Only a second press is held back, until the first is answered.
  expect(button("撤销授权").disabled).toBe(true);
  answer();
  await waitFor(() => expect(button("撤销授权").disabled).toBe(false));
});

test("reopens the bound remote instance and displays the service-owned environment", async () => {
  const account = {
    ...snapshot.ctp_connections![0],
    trading: { node: "machine-b", service: "account-b" },
  };
  const sent: unknown[] = [];
  const context: TerminalContext = {
    snapshot: { ...snapshot, ctp_connections: [account], live: {} },
    busy: false,
    error: "",
    query: async () => snapshot,
    navigate: () => {},
    openSettings: () => {},
    trade: async (...args) => {
      sent.push(args);
    },
  };
  const view = render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="remote">
      <LivePanel {...context} />
    </WorkspaceDraftScope>,
  );
  expect(screen.getByText("运行实例：machine-b / account-b")).toBeTruthy();
  fireEvent.click(button("启动交易服务"));
  await waitFor(() => expect(sent).toEqual([["live.open", { account: "account" }]]));
  view.rerender(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="remote">
      <LivePanel {...context} snapshot={{ ...snapshot, ctp_connections: [account] }} />
    </WorkspaceDraftScope>,
  );
  expect(screen.queryByRole("combobox", { name: "柜台环境" })).toBeNull();
  expect(screen.getByText("CTP 仿真")).toBeTruthy();
});

test("broker observation identity remains exact and incomplete coverage is visible without commands", () => {
  const sent: unknown[] = [];
  const context: TerminalContext = {
    snapshot,
    busy: false,
    error: "",
    query: async () => snapshot,
    navigate: () => {},
    openSettings: () => {},
    trade: async (...args) => {
      sent.push(args);
    },
  };
  const store = createWorkspaceDrafts();
  const view = render(
    <WorkspaceDraftScope store={store} namespace="facts">
      <LivePanel {...context} />
    </WorkspaceDraftScope>,
  );
  expect(screen.getByText("9007199254740993")).toBeTruthy();
  expect(screen.getByText("9007199254740995")).toBeTruthy();
  expect(screen.getByText("已纳入已知成交")).toBeTruthy();
  expect(
    screen.getByText(
      "资金、持仓、订单和成交可能来自不同时间；同步完成时间不代表原子账本或完整历史。",
    ),
  ).toBeTruthy();
  view.rerender(
    <WorkspaceDraftScope store={store} namespace="facts">
      <LivePanel
        {...context}
        snapshot={{
          ...snapshot,
          live: {
            account: {
              ...snapshot.live.account!,
              session: { ...session, phase: "disconnected", positions_reconciled: false },
            },
          },
        }}
      />
    </WorkspaceDraftScope>,
  );
  expect(screen.getByText("尚未核对已知成交")).toBeTruthy();
  expect(sent).toEqual([]);
});
