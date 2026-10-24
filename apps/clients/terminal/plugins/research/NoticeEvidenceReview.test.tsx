import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import type { TerminalContext } from "../contract";
import type { NoticeObject, TaskRecord, TaskResult } from "../../src/bridge/client";
import { NoticeEvidenceReview } from "./NoticeEvidenceReview";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const source: NoticeObject = {
  node: "stable-node",
  service: "intelligence-one",
  domain: "intelligence",
  kind: "intelligence_publication",
  id: "publication-one",
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
    trade: vi.fn(async () => {}),
    workspaceParams: { source: JSON.stringify(source), scope: "research" },
  };
}
function show(ctx: TerminalContext) {
  return render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="notice-review">
      <NoticeEvidenceReview {...ctx} />
    </WorkspaceDraftScope>,
  );
}
test("fixed publication and explicit UTC cutoff submit without a selected Data or trading operation", async () => {
  const ctx = context();
  show(ctx);
  fireEvent.change(screen.getByLabelText("公告审阅任务 ID"), { target: { value: "review-one" } });
  fireEvent.change(screen.getByLabelText("审阅截止时间（UTC）"), {
    target: { value: "2026-10-22T12:30:00.123" },
  });
  fireEvent.click(screen.getByRole("button", { name: "提交或确认原公告审阅" }));
  await waitFor(() =>
    expect(ctx.trade).toHaveBeenCalledWith("notice.review.submit", {
      id: "review-one",
      owner: {
        kind: "research",
        node_id: "local",
        service: "research",
        connection_id: "research-connection",
      },
      source,
      as_of_ms: String(Date.parse("2026-10-22T12:30:00.123Z")),
      use_scope: "research",
    }),
  );
  expect(ctx.trade).toHaveBeenCalledTimes(1);
  expect(ctx.query).not.toHaveBeenCalled();
  expect(screen.getByText(/不计算策略收益、预测效果或交易授权/)).toBeTruthy();
});
test("an unpublished preparation cannot become a Research publication source", () => {
  const ctx = context();
  ctx.workspaceParams = {
    source: JSON.stringify({ ...source, kind: "intelligence_preparation" }),
    scope: "research",
  };
  show(ctx);
  expect(screen.queryByRole("button", { name: "提交或确认原公告审阅" })).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
});
test("result visibility follows stable Research ownership and preserves time and prediction limitations", () => {
  const ctx = context();
  ctx.snapshot!.research_service!.owner_node_id = "stable-business-node";
  const task: TaskRecord = {
    kind: "notice_review",
    id: "review-one",
    state: "succeeded",
    attempt: 1,
    completed: 1,
    total: 1,
    error: "",
    result_digest: "b".repeat(64),
    trading_day: "",
    instrument: "SHFE/rb/2026-11",
    source_name: "intelligence-one",
    submission_sequence: 1,
    submitted_at_ms: 1,
    updated_at_ms: 1,
  };
  const owner = { node: "stable-business-node", service: "research", domain: "research" as const };
  const content = {
    ...source,
    domain: "data" as const,
    service: "data-one",
    kind: "intelligence_content",
    version: "c".repeat(64),
  };
  const evidence: Extract<TaskResult, { kind: "notice_review" }> = {
    kind: "notice_review",
    id: task.id,
    task,
    input_sha256: "d".repeat(64),
    experiment: {
      owner,
      source,
      content,
      as_of_ms: "599",
      use_scope: "research",
      contract: task.instrument,
      material: { ...content, kind: "material_acquisition" },
      verification_program: "e".repeat(64),
      program_artifact: "f".repeat(64),
    },
    result: {
      owner,
      source,
      content,
      program_artifact: "f".repeat(64),
      as_of_ms: "599",
      use_scope: "research",
      contract: task.instrument,
      effective_day: "2026-10-24",
      margin_ratio: "0.12",
      planned: true,
      source_published_at_ms: null,
      acquired_at_ms: "200",
      processed_at_ms: "300",
      verified_at_ms: "400",
      stored_at_ms: "500",
      available_at_ms: "600",
      published_by_cutoff: false,
      strict_history_eligible: false,
      limitations: [
        "fixed_announcement_evidence_only",
        "no_prediction_validation",
        "source_publication_time_unknown",
        "post_hoc_publication",
        "publication_after_cutoff",
      ],
    },
  };
  ctx.snapshot!.task_result = evidence;
  const view = show(ctx);
  expect(screen.getByRole("region", { name: "公告证据审阅结果" })).toBeTruthy();
  expect(screen.getByText("截至所选时点不可用")).toBeTruthy();
  expect(screen.getByText("不满足严格历史使用条件")).toBeTruthy();
  expect(screen.getByText("未验证预测有效性")).toBeTruthy();
  expect(screen.getByText("来源发布时间未知")).toBeTruthy();
  evidence.result.owner = { ...owner, node: "another-node" };
  view.rerender(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="notice-review">
      <NoticeEvidenceReview {...ctx} />
    </WorkspaceDraftScope>,
  );
  expect(screen.queryByRole("region", { name: "公告证据审阅结果" })).toBeNull();
});

test("explicit source reconciliation keeps task ownership and never borrows another Research receipt", async () => {
  const ctx = context();
  const service = ctx.snapshot!.research_service!;
  service.owner_node_id = "stable-node";
  ctx.snapshot!.notice_custody = {
    source,
    consumer: {
      node: "stable-node",
      service: service.service,
      domain: "research",
      kind: "notice_review",
      id: "review-one",
      version: "b".repeat(64),
    },
    use_scope: "research",
    phase: "confirmed",
    created_at_ms: "1",
    updated_at_ms: "2",
  };
  const store = createWorkspaceDrafts();
  const view = render(
    <WorkspaceDraftScope store={store} namespace="notice-reconcile">
      <NoticeEvidenceReview {...ctx} />
    </WorkspaceDraftScope>,
  );
  fireEvent.change(screen.getByLabelText("需要重核的任务 ID"), { target: { value: "review-one" } });
  expect(screen.getByText("源引用关系已确认")).toBeTruthy();
  expect(screen.getByText(/不是持续在线证明/)).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "重核并续办源引用" }));
  await waitFor(() =>
    expect(ctx.trade).toHaveBeenCalledWith("notice.custody.reconcile", {
      id: "review-one",
      owner: {
        kind: "research",
        node_id: "local",
        service: service.service,
        connection_id: service.connection_id,
      },
    }),
  );
  expect(ctx.query).not.toHaveBeenCalled();
  const changed = {
    ...ctx,
    snapshot: {
      ...ctx.snapshot!,
      research_service: { ...service, owner_node_id: "another-stable-node" },
    },
  };
  view.rerender(
    <WorkspaceDraftScope store={store} namespace="notice-reconcile">
      <NoticeEvidenceReview {...changed} />
    </WorkspaceDraftScope>,
  );
  expect(screen.queryByText("源引用关系已确认")).toBeNull();
});
