import { create } from "@bufbuild/protobuf";
import { render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { ErrorCode } from "../../src/generated/asterion/common_pb";
import { AcquisitionTaskSchema, Table, TaskState } from "../../src/generated/asterion/warehouse_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { openPopover } from "../../src/testing/popover";
import { Activity, groupTasks } from "./Activity";
import { feature } from "./feature";

registerTerminalFeatures([feature]);

test("cancelling the remaining tables leaves a partially completed batch cancelled", () => {
  const tasks = [TaskState.SUCCEEDED, TaskState.CANCELLED].map((state, index) =>
    create(AcquisitionTaskSchema, {
      request: {
        id: `update-1-${index ? "settlement" : "daily"}`,
        table: index ? Table.SETTLEMENT : Table.DAILY,
      },
      state,
    }),
  );
  expect(groupTasks(tasks).map(batch => batch.state)).toEqual([TaskState.CANCELLED]);
});

test("automatic parameter and report tasks appear in activity only when they stop", () => {
  for (const [id, table] of [
    ["contracts-1", Table.CONTRACTS],
    ["price-ticks-1", Table.PRICE_TICKS],
    ["update-1-settlement", Table.SETTLEMENT],
    ["update-1-calendar", Table.CALENDAR],
    ["update-1-inventory", Table.INVENTORY],
  ] as const) {
    const task = (state: TaskState) =>
      create(AcquisitionTaskSchema, { request: { id, table }, state });
    expect(groupTasks([task(TaskState.RUNNING)])).toEqual([]);
    expect(groupTasks([task(TaskState.SUCCEEDED)])).toEqual([]);
    expect(groupTasks([task(TaskState.FAILED)])).toHaveLength(1);
  }
});

test("cancelling an update also stops the hidden tables queued with it", () => {
  const [batch] = groupTasks([
    create(AcquisitionTaskSchema, {
      request: { id: "update-1-daily", table: Table.DAILY },
      state: TaskState.RUNNING,
    }),
    create(AcquisitionTaskSchema, {
      request: { id: "update-1-settlement", table: Table.SETTLEMENT },
      state: TaskState.QUEUED,
    }),
  ]);
  expect(batch!.members).toHaveLength(1);
  expect(batch!.activeIds).toEqual(["update-1-daily", "update-1-settlement"]);
});

test("a recorded failed acquisition retains its error details when activity is opened", () => {
  render(
    <Activity
      tasks={[
        create(AcquisitionTaskSchema, {
          request: { id: "update-1-daily", table: Table.DAILY },
          state: TaskState.FAILED,
          error: { code: ErrorCode.OPERATION_FAILED, message: "supplier request denied" },
        }),
      ]}
      versions={[]}
      shown={20}
      refresh={vi.fn()}
      openVersion={vi.fn()}
    />,
  );
  openPopover(screen.getByRole("button", { name: "最近活动" }));
  const details = screen.getByText("错误详情").closest("details")!;
  expect(details.open).toBe(false);
  expect(details.querySelector("[title]")?.getAttribute("title")).toBe(
    "operation_failed: supplier request denied",
  );
});

test("weekly report dates do not become missing daily partitions, including in mixed updates", () => {
  render(
    <Activity
      tasks={[
        ...["acq-weekly", "update-2-inventory"].map(id =>
          create(AcquisitionTaskSchema, {
            request: { id, table: Table.INVENTORY },
            state: TaskState.FAILED,
            items: 20n,
            itemsDone: 20n,
            itemsEmpty: 16n,
          }),
        ),
        create(AcquisitionTaskSchema, {
          request: { id: "update-2-daily", table: Table.DAILY },
          state: TaskState.SUCCEEDED,
          items: 10n,
          itemsDone: 10n,
          itemsEmpty: 2n,
        }),
      ]}
      versions={[]}
      shown={20}
      refresh={vi.fn()}
      openVersion={vi.fn()}
    />,
  );
  openPopover(screen.getByRole("button", { name: "最近活动" }));
  const weekly = screen.getByText("采集 库存周报").closest("tr")!;
  expect(within(weekly).getByText("失败")).toBeTruthy();
  expect(weekly.textContent).toContain("100% · 20 / 20 个分区 · 周报按报告日提供");
  const update = screen.getByText("更新到最新").closest("tr")!;
  expect(update.textContent).toContain(
    "100% · 30 / 30 个分区 · 2 个分区暂无数据 · 周报按报告日提供",
  );
});

test("an inventory task that stored no report says so", () => {
  render(
    <Activity
      tasks={[
        create(AcquisitionTaskSchema, {
          request: { id: "acq-weekly", table: Table.INVENTORY },
          state: TaskState.FAILED,
          items: 20n,
          itemsDone: 20n,
          itemsEmpty: 20n,
        }),
      ]}
      versions={[]}
      shown={20}
      refresh={vi.fn()}
      openVersion={vi.fn()}
    />,
  );
  openPopover(screen.getByRole("button", { name: "最近活动" }));
  expect(screen.getByText("采集 库存周报").closest("tr")!.textContent).toContain(
    "100% · 20 / 20 个分区 · 没有新周报",
  );
});
