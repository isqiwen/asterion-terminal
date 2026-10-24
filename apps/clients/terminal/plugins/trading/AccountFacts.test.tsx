import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { TradingFactsStatus, Snapshot } from "../../src/bridge/client";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import type { TerminalContext } from "../contract";
import { AccountFacts } from "./AccountFacts";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const reference: TradingFactsStatus["reference"] = {
  node: "node-one",
  service: "trading-one",
  domain: "trading",
  kind: "trading_facts",
  id: "fixed-one",
  version: "a".repeat(64),
};
const status: TradingFactsStatus = {
  id: "fixed-one",
  reference,
  state: "published",
  admitted_at_ms: "1790000000000",
  published_at_ms: "1790000000001",
  capture: {
    reference,
    account_id: "account",
    environment: "simulation",
    instance: "old-instance",
    captured_at_ms: "1790000000000",
    broker_sequence: "9007199254740993",
    connection_generation: "9007199254740995",
    synchronized_ms: "0",
    phase: "disconnected",
    error_code: 0,
    trading_day: "",
    positions_reconciled: false,
    recovery_required: false,
    policy_revision: "policy",
    risk_artifact: "b".repeat(64),
    complete_history: false,
    atomic_ledger_cut: false,
    missing_evidence: ["cashflow_history"],
    funds: null,
    positions: [],
    orders: [],
    trades: [],
    unconfirmed: [],
  },
};
function context(
  reply: (method: string, params: unknown) => Promise<Snapshot>,
  calls: unknown[],
): TerminalContext {
  return {
    snapshot: readySnapshot(),
    busy: false,
    error: "",
    navigate: () => {},
    openSettings: () => {},
    query: async (...[method, params]) => {
      calls.push([method, params]);
      return reply(method, params);
    },
    trade: async (...args) => {
      calls.push(args);
    },
  };
}
test("fixed account evidence is explicit, precisely read and never connects or authorizes trading", async () => {
  const calls: unknown[] = [];
  const ctx = context(async () => ({ ...readySnapshot(), trading_facts: status }), calls);
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="facts">
      <AccountFacts context={ctx} account="account" scope="account/one" />
    </WorkspaceDraftScope>,
  );
  fireEvent.change(screen.getByLabelText("账户事实操作 ID"), { target: { value: "fixed-one" } });
  expect(
    (screen.getByRole("button", { name: "固定或恢复原操作" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  fireEvent.click(screen.getByLabelText("确认保留本次固定事实及覆盖限制"));
  fireEvent.click(screen.getByRole("button", { name: "固定或恢复原操作" }));
  await screen.findByText("账户事实已发布");
  expect(screen.getByText("9007199254740993")).toBeTruthy();
  expect(screen.getByText("9007199254740995")).toBeTruthy();
  expect(screen.getByText("未取得资金观察，不能当作零余额")).toBeTruthy();
  expect(calls).toEqual([
    ["live.facts.capture", { account: "account", id: "fixed-one" }],
    ["live.facts.status", { account: "account", id: "fixed-one" }],
  ]);
  const navigate = vi.fn();
  ctx.navigate = navigate;
  fireEvent.click(screen.getByRole("button", { name: "在 Research 审阅此固定事实" }));
  expect(navigate).toHaveBeenCalledWith("workspace.account-review", {
    page: "research",
    params: { source: JSON.stringify(reference), account: "account", environment: "simulation" },
  });
  fireEvent.click(screen.getByRole("button", { name: "读取此固定事实" }));
  await waitFor(() =>
    expect(calls.at(-1)).toEqual(["live.facts.read", { account: "account", reference }]),
  );
});
test("an account switch discards an old pending response without issuing another operation", async () => {
  const calls: unknown[] = [];
  let resolve!: (reply: Snapshot) => void;
  const ctx = context(
    () =>
      new Promise(answer => {
        resolve = answer;
      }),
    calls,
  );
  const store = createWorkspaceDrafts();
  const view = render(
    <WorkspaceDraftScope store={store} namespace="facts">
      <AccountFacts context={ctx} account="account" scope="account/one" />
    </WorkspaceDraftScope>,
  );
  fireEvent.change(screen.getByLabelText("账户事实操作 ID"), { target: { value: "fixed-one" } });
  fireEvent.click(screen.getByRole("button", { name: "检查原事实操作" }));
  view.rerender(
    <WorkspaceDraftScope store={store} namespace="facts">
      <AccountFacts context={ctx} account="another" scope="another/two" />
    </WorkspaceDraftScope>,
  );
  resolve({ ...readySnapshot(), trading_facts: status });
  await waitFor(() => expect(screen.queryByText("账户事实已发布")).toBeNull());
  expect(calls).toHaveLength(1);
});
