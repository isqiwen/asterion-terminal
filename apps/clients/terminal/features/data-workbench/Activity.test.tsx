import { create } from "@bufbuild/protobuf";
import { render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { ErrorCode } from "../../src/generated/asterion/common_pb";
import {
  AcquisitionTaskSchema,
  DatasetChangeSchema,
  Table,
  TaskState,
} from "../../src/generated/asterion/warehouse_pb";
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

test("automatic tasks are one cancellable row while running and leave nothing on success", () => {
  const task = (id: string, table: Table, state: TaskState) =>
    create(AcquisitionTaskSchema, { request: { id, table }, state });
  const [row, ...others] = groupTasks([
    task("price-ticks-1", Table.PRICE_TICKS, TaskState.QUEUED),
    task("update-1-settlement", Table.SETTLEMENT, TaskState.RUNNING),
  ]);
  expect(others).toEqual([]);
  expect(row!.primary.request!.table).toBe(Table.SETTLEMENT);
  expect(row!.activeIds).toEqual(["price-ticks-1", "update-1-settlement"]);
  expect(groupTasks([task("contracts-1", Table.CONTRACTS, TaskState.SUCCEEDED)])).toEqual([]);
  expect(groupTasks([task("contracts-1", Table.CONTRACTS, TaskState.FAILED)])).toHaveLength(1);
});

test("a version made only by automatic work is not listed in activity", () => {
  const version = (id: string, tables: Table[]) =>
    create(DatasetChangeSchema, { dataset: { version: id, createdMs: 1n }, added: 1, tables });
  render(
    <Activity
      tasks={[]}
      versions={[
        version("marketbars", [Table.DAILY, Table.SETTLEMENT]),
        version("automatic", [Table.SETTLEMENT]),
      ]}
      shown={20}
      refresh={vi.fn()}
      openVersion={vi.fn()}
    />,
  );
  openPopover(screen.getByRole("button", { name: "最近活动" }));
  expect(screen.getByText("marketba")).toBeTruthy();
  expect(screen.queryByText("automati")).toBeNull();
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
  const update = screen.getByText("更新").closest("tr")!;
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
