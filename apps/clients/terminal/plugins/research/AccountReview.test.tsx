import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import type { TerminalContext } from "../contract";
import { AccountReview } from "./AccountReview";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const source = {
  node: "node-one",
  service: "trading-one",
  domain: "trading",
  kind: "trading_facts",
  id: "fixed-one",
  version: "a".repeat(64),
};
function context(): TerminalContext {
  const snapshot = readySnapshot();
  snapshot.data = null;
  return {
    snapshot,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
    query: vi.fn(async () => snapshot),
    trade: vi.fn(async () => undefined),
    workspaceParams: {
      source: JSON.stringify(source),
      account: "account-one",
      environment: "simulation",
    },
  };
}
test("fixed Trading source submits to the selected Research without Data or trading commands", async () => {
  const ctx = context();
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="review">
      <AccountReview {...ctx} />
    </WorkspaceDraftScope>,
  );
  fireEvent.change(screen.getByLabelText("账户审阅任务 ID"), { target: { value: "review-one" } });
  fireEvent.click(screen.getByRole("button", { name: "提交或确认原审阅" }));
  await waitFor(() =>
    expect(ctx.trade).toHaveBeenCalledWith("account.review.submit", {
      id: "review-one",
      source,
      account_id: "account-one",
      environment: "simulation",
      owner: {
        kind: "research",
        node_id: "local",
        service: "research",
        connection_id: "research-connection",
      },
    }),
  );
  expect(ctx.trade).toHaveBeenCalledTimes(1);
  expect(ctx.query).not.toHaveBeenCalled();
  expect(screen.getByText(/不计算完整账本绩效/)).toBeTruthy();
});
test("invalid or absent source opens existing records without a new submission", () => {
  const ctx = context();
  ctx.workspaceParams = { source: "invalid" };
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="review">
      <AccountReview {...ctx} />
    </WorkspaceDraftScope>,
  );
  expect(screen.queryByRole("button", { name: "提交或确认原审阅" })).toBeNull();
  expect(screen.getByText(/已有任务可直接查看结果/)).toBeTruthy();
  expect(ctx.trade).not.toHaveBeenCalled();
});

test("result visibility uses stable business ownership rather than the Manager local alias", () => {
  const ctx = context();
  const snapshot = ctx.snapshot!;
  snapshot.research_service!.owner_node_id = "stable-business-node";
  snapshot.task_result = {
    kind: "account_review",
    id: "review-one",
    input_sha256: "b".repeat(64),
    task: {
      kind: "account_review",
      id: "review-one",
      state: "succeeded",
      attempt: 1,
      completed: 1,
      total: 1,
      error: "",
      result_digest: "c".repeat(64),
      trading_day: "",
      instrument: "account-one",
      source_name: "trading-one",
      submission_sequence: 1,
      submitted_at_ms: 1,
      updated_at_ms: 1,
    },
    experiment: {
      owner: { node: "stable-business-node", service: "research", domain: "research" },
      program_artifact: "d".repeat(64),
      evidence: {
        id: "fixed-one",
        reference: { ...source, domain: "trading", kind: "trading_facts" },
        state: "published",
        admitted_at_ms: "1",
      },
    },
    result: {
      owner: { node: "stable-business-node", service: "research", domain: "research" },
      source: { ...source, domain: "trading", kind: "trading_facts" },
      account_id: "account-one",
      environment: "simulation",
      program_artifact: "d".repeat(64),
      conclusion: "evidence_insufficient",
      position_records: 0,
      order_records: 0,
      trade_records: 0,
      unconfirmed_orders: 0,
      funds_present: false,
      positions_reconciled: false,
      broker_ready: false,
      recovery_required: false,
      limitations: ["funds_observation_missing"],
    },
  };
  const drafts = createWorkspaceDrafts();
  const view = render(
    <WorkspaceDraftScope store={drafts} namespace="review">
      <AccountReview {...ctx} />
    </WorkspaceDraftScope>,
  );
  expect(screen.getByRole("region", { name: "账户观察审阅结果" })).toBeTruthy();
  expect(screen.getByText("未取得资金观察，不能当作零余额")).toBeTruthy();
  snapshot.research_service!.owner_node_id = "another-business-node";
  view.rerender(
    <WorkspaceDraftScope store={drafts} namespace="review">
      <AccountReview {...ctx} />
    </WorkspaceDraftScope>,
  );
  expect(screen.queryByRole("region", { name: "账户观察审阅结果" })).toBeNull();
});
