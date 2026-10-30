import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { readySnapshot } from "../../src/testing/core";
import type { TableRefreshPlan, TableRevision, TaskRecord } from "../../src/bridge/client";
import { TableRefreshPanel } from "./TableRefreshPanel";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
const source = {
  table: "futures.daily",
  schema: 1,
  source: "fixture.daily.v1",
  plugin_id: "test.independent.c",
  venues: ["SHFE"],
};
const revision: TableRevision = {
  table: source.table,
  source: source.source,
  exchange: "SHFE",
  period: "2024-03-01",
  schema: 1,
  sequence: 3,
  rows: 1,
  bytes: 20,
  acquired_at_ns: "1709251200000000000",
  provider_artifact: "a".repeat(64),
  snapshot: "b".repeat(64),
  content: "c".repeat(64),
};
const target = {
  partition: { table: revision.table, exchange: revision.exchange, period: revision.period },
  source: revision.source,
  previous_snapshot: revision.snapshot,
  previous_content: revision.content,
  provider_artifact: "a".repeat(64),
};
const plan: TableRefreshPlan = {
  id: "plan",
  query: {
    provider: source.plugin_id,
    source: source.source,
    table: revision.table,
    exchange: revision.exchange,
    period: revision.period,
    previous_snapshot: revision.snapshot,
    requests_per_minute: 60,
  },
  previous: revision,
  update: {
    provider: source.plugin_id,
    tables: [revision.table],
    begin_day: revision.period,
    end_day: revision.period,
    requests_per_minute: 60,
    refresh: target,
  },
};
const tracked = (state: TaskRecord["state"]): TaskRecord => ({
  id: "tracked-refresh",
  kind: "table_update",
  state,
  attempt: 2,
  completed: 100,
  total: 100,
  error: "",
  result_digest: "digest",
  trading_day: revision.period,
  instrument: revision.table,
  source_name: source.plugin_id,
  submission_sequence: 1,
  submitted_at_ms: 1,
  updated_at_ms: 2,
  table_refresh: target,
});
function context(task?: TaskRecord) {
  const snapshot = readySnapshot();
  snapshot.data!.sources = [
    {
      id: "fixture.minutes",
      name: "Fixture",
      plugin_id: source.plugin_id,
      normalization: "fixture.v1",
      timezone: "Asia/Shanghai",
      timestamp_semantics: "bar_end",
      venues: ["SHFE"],
      intervals: [1],
      max_requests_per_minute: 60,
      credential_required: false,
    },
  ];
  if (task) snapshot.data!.tasks = [task];
  return {
    snapshot,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
    trade: vi.fn(async () => {}),
    query: vi.fn(async () => ({ ...snapshot, table_refresh_plan: plan })),
  };
}
test("selects one fixed existing partition, previews without trade, then requires explicit confirmation", async () => {
  const ctx = context();
  render(
    <TableRefreshPanel
      context={ctx}
      source={source}
      revision={revision}
      onReadCurrent={() => {}}
    />,
  );
  expect(ctx.query).not.toHaveBeenCalledWith("data.tables.refresh.tasks", {});
  expect(ctx.trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "预览重新获取" }));
  await screen.findByRole("button", { name: "确认重新获取此分区" });
  expect(ctx.query).toHaveBeenCalledWith("data.tables.refresh.plan", plan.query);
  expect(ctx.trade).not.toHaveBeenCalled();
  expect(screen.getByText(/预览未下载数据/)).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "确认重新获取此分区" }));
  await waitFor(() =>
    expect(ctx.trade).toHaveBeenCalledWith(
      "data.tables.refresh.submit",
      expect.objectContaining({ query: plan.query, plan_id: "plan", token: "" }),
    ),
  );
  expect(ctx.trade).not.toHaveBeenCalledWith("data.tables.update.submit", expect.anything());
});
test("running and failed attempts expose only the existing explicit cancel and retry actions", async () => {
  const ctx = context(tracked("running"));
  const view = render(
    <TableRefreshPanel
      context={ctx}
      source={source}
      revision={revision}
      onReadCurrent={() => {}}
    />,
  );
  fireEvent.click(screen.getByRole("button", { name: "停止重新获取" }));
  await waitFor(() =>
    expect(ctx.trade).toHaveBeenCalledWith("task.action", {
      owner: {
        kind: "data",
        node_id: ctx.snapshot.data!.node_id,
        service: ctx.snapshot.data!.service,
        connection_id: ctx.snapshot.data!.connection_id,
      },
      id: "tracked-refresh",
      action: "cancel",
    }),
  );
  const failed = context(tracked("failed"));
  view.rerender(
    <TableRefreshPanel
      context={failed}
      source={source}
      revision={revision}
      onReadCurrent={() => {}}
    />,
  );
  fireEvent.click(screen.getByRole("button", { name: "重试此任务" }));
  await waitFor(() =>
    expect(failed.trade).toHaveBeenCalledWith("task.action", {
      owner: {
        kind: "data",
        node_id: ctx.snapshot.data!.node_id,
        service: ctx.snapshot.data!.service,
        connection_id: ctx.snapshot.data!.connection_id,
      },
      id: "tracked-refresh",
      action: "retry",
    }),
  );
  expect(screen.getByText(/已发布的修订不会回退/)).toBeTruthy();
});
test("reopening a partition finds its persisted refresh target and exact unchanged receipt", async () => {
  const task = tracked("succeeded");
  const ctx = context(task);
  ctx.query = vi.fn(async () => ({
    ...ctx.snapshot,
    table_refresh_plan: plan,
    task_result: {
      id: task.id,
      kind: "table_update" as const,
      task,
      experiment: plan.update,
      result: {
        stored: 0,
        unchanged: 1,
        rejected: [],
        unpublished: 0,
        unpublished_examples: [],
        waiting: [],
        snapshot: revision.snapshot,
        refreshed_revision: revision,
      },
    },
  }));
  render(
    <TableRefreshPanel
      context={ctx}
      source={source}
      revision={revision}
      onReadCurrent={() => {}}
    />,
  );
  await screen.findByText("重新获取完成：内容未变，未新增修订");
  expect(screen.getByText(/tracked-refresh/)).toBeTruthy();
  expect(ctx.query).toHaveBeenCalledWith("task.result.read", {
    owner: {
      kind: "data",
      node_id: ctx.snapshot.data!.node_id,
      service: ctx.snapshot.data!.service,
      connection_id: ctx.snapshot.data!.connection_id,
    },
    id: task.id,
  });
  expect(ctx.trade).not.toHaveBeenCalled();
});

test("declared exchange exclusion blocks submission without inventing a historical date boundary", () => {
  const ctx = context();
  render(
    <TableRefreshPanel
      context={ctx}
      source={{ ...source, venues: ["DCE"] }}
      revision={revision}
      onReadCurrent={() => {}}
    />,
  );
  expect(screen.getByRole("alert").textContent).toContain("当前来源声明不支持所选交易所");
  expect(screen.getByRole("button", { name: "预览重新获取" })).toHaveProperty("disabled", true);
  expect(ctx.query).not.toHaveBeenCalledWith("data.tables.refresh.tasks", {});
  expect(ctx.trade).not.toHaveBeenCalled();
});
test("empty response preserves unknown coverage and offers read then preview rather than retrying a succeeded task", async () => {
  const task = tracked("succeeded");
  const ctx = context(task);
  ctx.query = vi.fn(async () => ({
    ...ctx.snapshot,
    table_refresh_plan: plan,
    task_result: {
      id: task.id,
      kind: "table_update" as const,
      task,
      experiment: plan.update,
      result: {
        stored: 0,
        unchanged: 0,
        rejected: [],
        unpublished: 1,
        unpublished_examples: [target.partition],
        waiting: [],
        snapshot: "",
      },
    },
  }));
  const onReadCurrent = vi.fn();
  render(
    <TableRefreshPanel
      context={ctx}
      source={source}
      revision={revision}
      onReadCurrent={onReadCurrent}
    />,
  );
  await screen.findByText(/本次来源响应为空，原因与历史覆盖均未知/);
  expect(screen.queryByRole("button", { name: "重试此任务" })).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "读取当前分区" }));
  expect(onReadCurrent).toHaveBeenCalledOnce();
  fireEvent.click(screen.getByRole("button", { name: "预览重新获取" }));
  await screen.findByRole("button", { name: "确认重新获取此分区" });
  expect(ctx.trade).not.toHaveBeenCalled();
});
test.each([
  ["unavailable", "来源不可用或资源受限"],
  [undefined, "现有证据不能确定是否为临时故障或历史覆盖问题"],
])(
  "failure code %s explains recovery without parsing diagnostics or automatically retrying",
  async (error_code, explanation) => {
    const ctx = context({ ...tracked("failed"), error_code, error: "fixture source unavailable" });
    render(
      <TableRefreshPanel
        context={ctx}
        source={source}
        revision={revision}
        onReadCurrent={() => {}}
      />,
    );
    expect(screen.getByRole("status").textContent).toContain(explanation);
    expect(screen.getByText(/所选日期的来源历史覆盖：未知/)).toBeTruthy();
    expect(ctx.trade).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole("button", { name: "重试此任务" }));
    await waitFor(() =>
      expect(ctx.trade).toHaveBeenCalledWith("task.action", {
        owner: {
          kind: "data",
          node_id: ctx.snapshot.data!.node_id,
          service: ctx.snapshot.data!.service,
          connection_id: ctx.snapshot.data!.connection_id,
        },
        id: "tracked-refresh",
        action: "retry",
      }),
    );
  },
);

test("stored partition preview belongs to Data even while Research is offline", async () => {
  const ctx = context();
  ctx.snapshot.research_service!.online = false;
  render(
    <TableRefreshPanel
      context={ctx}
      source={source}
      revision={revision}
      onReadCurrent={() => {}}
    />,
  );
  const preview = screen.getByRole("button", { name: "预览重新获取" }) as HTMLButtonElement;
  expect(preview.disabled).toBe(false);
  fireEvent.click(preview);
  await screen.findByText("原分区修订", { exact: false });
  expect(
    (screen.getByRole("button", { name: "确认重新获取此分区" }) as HTMLButtonElement).disabled,
  ).toBe(false);
  expect(ctx.trade).not.toHaveBeenCalled();
  expect(ctx.query).not.toHaveBeenCalledWith("data.tables.refresh.tasks", {});
});
