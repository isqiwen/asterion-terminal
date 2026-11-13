import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import { AcquisitionTaskSchema, Table, TaskState } from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { Overview } from "./Overview";

const request = vi.hoisted(() => vi.fn());
vi.mock("../../src/bridge/warehouse-data", () => ({
  warehouseData: request,
}));
vi.mock("../../src/i18n", async importOriginal => ({
  ...(await importOriginal<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
afterEach(() => vi.restoreAllMocks());
const context = () =>
  ({
    navigate: vi.fn(),
    snapshot: { warehouse_endpoint: { host: "market", port: 7450, fingerprint: "f" } },
  }) as unknown as TerminalContext;
const task = (id: string, startedMs = 1n) =>
  create(AcquisitionTaskSchema, {
    request: { id, table: Table.DAILY, firstDay: "2026-10-30", lastDay: "2026-10-30" },
    state: TaskState.RUNNING,
    items: 1n,
    startedMs,
  });
const older = "a".repeat(64);
const current = "b".repeat(64);

// Daily bars two trading days behind, settlement current, minutes never taken.
function serve(tasks: (afterId: string) => { tasks: unknown[]; nextId: string }) {
  request.mockImplementation(async (body: { case: string; value: { afterId?: string } }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables: [Table.DAILY, Table.MINUTES], exchanges: ["SHFE"] };
      case "getDatasetSummary":
        return {
          dataset: { version: current, createdMs: 3n },
          tables: [
            {
              table: Table.DAILY,
              rows: 5n,
              firstDay: "2026-10-12",
              lastDay: "2026-11-10",
              expectedLastDay: "2026-11-12",
              missing: 2n,
            },
            {
              table: Table.SETTLEMENT,
              rows: 3n,
              firstDay: "2026-11-12",
              lastDay: "2026-11-12",
              expectedLastDay: "2026-11-12",
            },
          ],
        };
      case "listAcquisitionTasks":
        return tasks(body.value.afterId ?? "");
      case "listDatasetVersions":
        return {
          versions: [
            { dataset: { version: current, createdMs: 3n }, added: 1 },
            { dataset: { version: older, createdMs: 2n }, added: 2 },
          ],
          nextVersion: "",
        };
      case "startUpdate":
        return { tasks: [task("update-1-daily"), task("update-1-minutes")] };
      default:
        return {};
    }
  });
}

test("each table's state is shown against its calendar, and an update brings it current", async () => {
  serve(() => ({ tasks: [], nextId: "" }));
  render(<Overview {...context()} />);
  const status = await screen.findByRole("table", { name: "数据状态" });
  const row = (name: string) => within(status).getByRole("row", { name: new RegExp(`^${name}`) });
  expect(row("日线").textContent).toContain("缺 2 个交易日");
  expect(row("日线").textContent).toContain("应到 2026-11-12");
  expect(row("结算参数").textContent).toContain("最新");
  expect(row("分钟线").textContent).toContain("未采集");
  fireEvent.click(screen.getByRole("button", { name: "更新到最新" }));
  await screen.findByText("已提交 2 个更新任务");
  expect(request).toHaveBeenCalledWith({ case: "startUpdate", value: {} });
});

test("another client's task shows on the next poll and can be cancelled", async () => {
  let poll: () => void = () => {};
  const interval = globalThis.setInterval;
  vi.spyOn(globalThis, "setInterval").mockImplementation((callback, delay, ...args) => {
    if (delay === 3000) poll = callback as () => void;
    return interval(callback, delay, ...args);
  });
  let remote = false;
  serve(() => ({ tasks: remote ? [task("other-client", 4n)] : [], nextId: "" }));
  render(<Overview {...context()} />);
  await screen.findByRole("table", { name: "最近活动" });
  remote = true;
  await act(async () => {
    poll();
  });
  fireEvent.click(await screen.findByRole("button", { name: "取消" }));
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      case: "cancelAcquisition",
      value: { id: "other-client" },
    }),
  );
});

test("each refresh reads one page of tasks; earlier activity is read on request", async () => {
  const page = Array.from({ length: 20 }, (_, i) => task(`task-${i}`, BigInt(100 - i)));
  serve(afterId =>
    afterId ? { tasks: [task("oldest", 0n)], nextId: "" } : { tasks: page, nextId: "task-19" },
  );
  render(<Overview {...context()} />);
  const activity = await screen.findByRole("table", { name: "最近活动" });
  const reads = () =>
    request.mock.calls.filter(([body]) => body.case === "listAcquisitionTasks").map(([b]) => b);
  await waitFor(() => expect(within(activity).getAllByRole("row")).toHaveLength(20));
  expect(reads().every(body => body.value.afterId === "")).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "更早的活动" }));
  // Twenty tasks, the two versions and the oldest task.
  await waitFor(() =>
    expect(
      within(screen.getByRole("table", { name: "最近活动" })).getAllByRole("row"),
    ).toHaveLength(23),
  );
  expect(reads().at(-1)).toEqual({ case: "listAcquisitionTasks", value: { afterId: "task-19" } });
  expect(screen.queryByRole("button", { name: "更早的活动" })).toBeNull();
});

test("a published version opens its coverage; the current one opens the latest", async () => {
  serve(() => ({ tasks: [], nextId: "" }));
  const props = context();
  render(<Overview {...props} />);
  const activity = await screen.findByRole("table", { name: "最近活动" });
  const version = (id: string) =>
    within(activity).getByRole("row", { name: new RegExp(id.slice(0, 8)) });
  fireEvent.click(within(version(older)).getByRole("button", { name: "查看" }));
  expect(props.navigate).toHaveBeenCalledWith("workspace.data-coverage", {
    params: { version: older },
  });
  fireEvent.click(within(version(current)).getByRole("button", { name: "查看" }));
  expect(props.navigate).toHaveBeenLastCalledWith("workspace.data-coverage", {});
});
