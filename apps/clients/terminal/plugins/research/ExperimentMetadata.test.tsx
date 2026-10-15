import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { TaskRecord } from "../../src/bridge/client";
import { BackendError } from "../contract";
import { ExperimentMetadata, emptyMetadata } from "./ExperimentMetadata";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const task: TaskRecord = {
  id: "derived",
  kind: "factor",
  state: "succeeded",
  attempt: 1,
  completed: 5,
  total: 5,
  error: "",
  result_digest: "a".repeat(64),
  trading_day: "",
  instrument: "SHFE/rb",
  source_name: "fixed",
  submission_sequence: 1,
  submitted_at_ms: 1,
  updated_at_ms: 1,
  metadata: {
    ...emptyMetadata,
    source_id: "original",
    source_instance: "research-task",
    source_result_digest: "b".repeat(64),
    data_choice: "fixed",
  },
};
test("failed saves and remounts retain notes; explicit refresh retains draft and saves against latest revision", async () => {
  const store = createWorkspaceDrafts(),
    snapshot = readySnapshot();
  const trade = vi
    .fn()
    .mockRejectedValueOnce(
      new BackendError("conflict", "experiment metadata changed; reload before saving"),
    )
    .mockResolvedValue(undefined);
  const query = vi.fn(async () => ({
    ...snapshot,
    task_description: {
      ...task,
      metadata: { ...task.metadata!, revision: 5, name: "other editor", note: "remote note" },
    },
  }));
  const editor = () => (
    <WorkspaceDraftScope store={store} namespace="description">
      <ExperimentMetadata
        task={task}
        connection="service"
        busy={false}
        trade={trade}
        query={query}
      />
    </WorkspaceDraftScope>
  );
  const first = render(editor());
  fireEvent.click(screen.getByText("名称、备注与来源"));
  fireEvent.change(screen.getByLabelText("实验名称"), { target: { value: "my experiment" } });
  fireEvent.change(screen.getByLabelText("实验备注"), {
    target: { value: "do not lose\nsecond line" },
  });
  fireEvent.click(screen.getByRole("button", { name: "保存描述" }));
  await screen.findByText(/实验描述已被其他操作修改/);
  expect(screen.queryByText("描述已保存")).toBeNull();
  first.unmount();
  render(editor());
  fireEvent.click(screen.getByText("名称、备注与来源"));
  expect((screen.getByLabelText("实验备注") as HTMLTextAreaElement).value).toBe(
    "do not lose\nsecond line",
  );
  fireEvent.click(screen.getByRole("button", { name: "读取最新描述（保留草稿）" }));
  await screen.findByText(/remote note/);
  expect((screen.getByLabelText("实验备注") as HTMLTextAreaElement).value).toBe(
    "do not lose\nsecond line",
  );
  fireEvent.click(screen.getByRole("button", { name: "保存描述" }));
  await screen.findByRole("status");
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(2));
  expect(trade.mock.calls[1]).toEqual([
    "task.metadata.edit",
    {
      id: "derived",
      expected_revision: 5,
      name: "my experiment",
      note: "do not lose\nsecond line",
    },
  ]);
  expect(query).toHaveBeenCalledWith("task.metadata.read", { id: "derived" });
  expect(screen.getByText(/research-task \/ original/)).toBeTruthy();
  expect(screen.getByText(/派生数据选择.*沿用原固定快照/)).toBeTruthy();
});
