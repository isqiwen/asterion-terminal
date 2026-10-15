import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { BackendError } from "../../src/i18n/errors";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { Factor } from "./Factor";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
test("an incompatible fixed input offers fresh selection without retrying or changing the old task", async () => {
  const snapshot = readySnapshot();
  snapshot.task_service!.tasks = [
    {
      kind: "factor",
      id: "legacy",
      state: "succeeded",
      attempt: 1,
      completed: 40,
      total: 40,
      error: "",
      result_digest: "b".repeat(64),
      trading_day: "",
      instrument: "SHFE/rb",
      source_name: "old",
      submission_sequence: 1,
      submitted_at_ms: 1,
      updated_at_ms: 1,
    },
  ];
  snapshot.task_service!.tasks.push({
    ...snapshot.task_service!.tasks[0],
    id: "failed-old",
    state: "failed",
    result_digest: "",
    error: "unsupported table series version",
  });
  const trade = vi.fn(async () => {
    throw new BackendError("invalid_request", "unsupported table series version");
  });
  const query = vi.fn(async () => snapshot);
  Object.defineProperty(HTMLElement.prototype, "scrollIntoView", {
    configurable: true,
    value: vi.fn(),
  });
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="recovery">
      <Factor
        snapshot={snapshot}
        busy={false}
        error=""
        openSettings={() => {}}
        trade={trade}
        query={query}
        navigate={() => {}}
      />
    </WorkspaceDraftScope>,
  );
  expect((screen.getByRole("button", { name: "重新运行" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  fireEvent.click(screen.getByRole("button", { name: "创建新实验" }));
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "查看结果" }));
  await screen.findByRole("region", { name: "重新创建实验" });
  expect(screen.getByText(/旧任务和固定输入文件已保留/).textContent).toContain(
    "新任务不是旧实验的恢复",
  );
  fireEvent.click(screen.getByRole("button", { name: "重新选择日线输入" }));
  await waitFor(() =>
    expect(screen.getByRole("button", { name: "全市场数据表" }).getAttribute("aria-pressed")).toBe(
      "true",
    ),
  );
  expect(trade).toHaveBeenCalledTimes(1);
  expect(trade).toHaveBeenCalledWith("task.result", { id: "legacy" });
  expect(snapshot.task_service!.tasks[0].state).toBe("succeeded");
});
