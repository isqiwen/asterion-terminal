import { create } from "@bufbuild/protobuf";
import { render, screen } from "@testing-library/react";
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
