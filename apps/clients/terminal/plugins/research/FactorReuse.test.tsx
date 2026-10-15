import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { FactorExperiment, TaskRecord } from "../../src/bridge/client";
import { FactorReuse } from "./FactorReuse";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
test("reuse requires an explicit fixed or new snapshot choice and creates a separate task", async () => {
  const snapshot = readySnapshot();
  const experiment: FactorExperiment = {
    version: 9,
    factor: "momentum",
    dataset_revision: "a".repeat(64),
    lookbacks: [2],
    horizon: 1,
    evaluation: { mode: "full_sample" },
    liquidity: null,
    series: [
      {
        kind: "table",
        data: {
          instrument: "SHFE/rb",
          snapshot: "b".repeat(64),
          daily_source: "fixture.daily",
          calendar_source: "fixture.calendar",
          begin_day: "2026-01-01",
          end_day: "2026-03-01",
          acquired_at_ns: "1",
          months: [],
          rolls: [],
          terms: 0,
          count: 40,
        },
      },
    ],
  };
  const task: TaskRecord = {
    kind: "factor",
    id: "original",
    state: "succeeded",
    attempt: 1,
    completed: 40,
    total: 40,
    error: "",
    result_digest: "d".repeat(64),
    trading_day: "",
    instrument: "SHFE/rb",
    source_name: "old",
    submission_sequence: 1,
    submitted_at_ms: 1,
    updated_at_ms: 1,
  };
  const original = JSON.stringify({ task, experiment });
  const run = vi.fn(async (_method: string, _params?: Record<string, unknown>) => true);
  const query = vi.fn(async () => ({
    ...snapshot,
    table_coverage: { snapshot: "c".repeat(64), sources: [], rows: [] },
  }));
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="reuse">
      <FactorReuse
        evidence={{ id: task.id, task, experiment }}
        snapshot={snapshot}
        busy={false}
        query={query}
        run={run}
      />
    </WorkspaceDraftScope>,
  );
  const create = screen.getByRole("button", { name: "创建独立因子任务" }) as HTMLButtonElement;
  expect(create.disabled).toBe(true);
  fireEvent.click(screen.getByLabelText("沿用原固定快照"));
  fireEvent.click(create);
  await waitFor(() => expect(run).toHaveBeenCalledTimes(1));
  expect(run.mock.calls[0][0]).toBe("factor.reuse");
  expect(run.mock.calls[0][1]).toEqual({
    id: expect.stringMatching(/^factor-/),
    source_id: "original",
    source_result_digest: task.result_digest,
    experiment,
    mode: "fixed",
    snapshot: "",
  });
  fireEvent.click(screen.getByLabelText("选择当前服务的新快照"));
  await screen.findByText("c".repeat(64));
  fireEvent.click(screen.getByRole("button", { name: "创建独立因子任务" }));
  await waitFor(() => expect(run).toHaveBeenCalledTimes(2));
  expect(run.mock.calls[1][1]).toMatchObject({
    source_id: "original",
    mode: "snapshot",
    snapshot: "c".repeat(64),
    experiment,
  });
  expect(run.mock.calls[1][1]?.id).not.toBe("original");
  expect(query).toHaveBeenCalledWith("data.tables.coverage", {});
  expect(JSON.stringify({ task, experiment })).toBe(original);
  fireEvent.click(screen.getByRole("button", { name: "修改回看窗口并派生" }));
  fireEvent.change(screen.getByLabelText("派生回看日线数"), { target: { value: "5" } });
  fireEvent.click(screen.getByLabelText("沿用原固定快照"));
  expect(
    (screen.getByRole("button", { name: "确认创建派生因子任务" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "核对参数派生" }));
  const review = screen.getByRole("region", { name: "参数派生确认" });
  expect(review.textContent).toContain("2 → 5");
  expect(review.textContent).toContain("fixture.daily");
  fireEvent.click(screen.getByRole("button", { name: "取消参数派生" }));
  expect(run).toHaveBeenCalledTimes(2);
  expect(screen.queryByRole("region", { name: "参数派生确认" })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "修改回看窗口并派生" }));
  fireEvent.change(screen.getByLabelText("派生回看日线数"), { target: { value: "5" } });
  fireEvent.click(screen.getByLabelText("沿用原固定快照"));
  fireEvent.click(screen.getByRole("button", { name: "核对参数派生" }));
  fireEvent.click(screen.getByRole("button", { name: "确认创建派生因子任务" }));
  await waitFor(() => expect(run).toHaveBeenCalledTimes(3));
  expect(run.mock.calls[2][1]).toMatchObject({
    source_id: "original",
    experiment,
    mode: "fixed",
    snapshot: "",
    lookbacks: [5],
  });
  expect(JSON.stringify({ task, experiment })).toBe(original);
});
