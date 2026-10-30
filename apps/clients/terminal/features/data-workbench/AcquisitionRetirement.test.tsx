import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { ResearchRetirement, TaskOwnerRef } from "../../src/bridge/client";
import { feature } from "./feature";
import { AcquisitionRetirement } from "./AcquisitionRetirement";
registerTerminalFeatures([feature]);
const owner: TaskOwnerRef = {
  kind: "data",
  node_id: "local",
  service: "data-one",
  connection_id: "connection",
};
const report: ResearchRetirement = {
  id: "acquisition",
  node: "stable-node",
  service: "data-one",
  version: "a".repeat(64),
  state: "available",
  active: false,
  admission_busy: false,
  consumers: [],
  source_release_confirmed: false,
  release_error: null,
  task: {
    id: "acquisition",
    kind: "table_update",
    state: "cancelled",
    attempt: 0,
    completed: 0,
    total: 1,
    error: "",
    result_digest: "",
    trading_day: "",
    instrument: "",
    source_name: "fixture",
    submission_sequence: 1,
    submitted_at_ms: 1,
    updated_at_ms: 1,
  },
};
const reply = (value: ResearchRetirement) => ({ ...readySnapshot(), task_retirement: value });
test("only explicit reviewed confirmation retires the exact Data owner without refund or deletion", async () => {
  const query = vi
    .fn()
    .mockResolvedValueOnce(reply(report))
    .mockResolvedValue(reply({ ...report, state: "retired" }));
  const trade = vi.fn(async () => undefined);
  render(
    <AcquisitionRetirement
      id="acquisition"
      owner={owner}
      busy={false}
      query={query}
      trade={trade}
    />,
  );
  expect(query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "维护采集记录" }));
  await screen.findByText("采集记录可退役");
  expect((screen.getByRole("button", { name: "确认采集退役" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("checkbox"));
  fireEvent.click(screen.getByRole("button", { name: "确认采集退役" }));
  await screen.findByText("采集记录已退役");
  expect(trade).toHaveBeenCalledExactlyOnceWith("task.retirement.execute", {
    id: "acquisition",
    owner,
  });
  expect(screen.getByText(/不退款、不删除产出数据/)).toBeTruthy();
});
test("active acquisition and incorrect service previews do not offer confirmation", async () => {
  const query = vi
    .fn()
    .mockResolvedValueOnce(reply({ ...report, state: "blocked", active: true }))
    .mockResolvedValue(reply({ ...report, service: "another-data" }));
  const trade = vi.fn();
  render(
    <AcquisitionRetirement
      id="acquisition"
      owner={owner}
      busy={false}
      query={query}
      trade={trade}
    />,
  );
  fireEvent.click(screen.getByRole("button", { name: "维护采集记录" }));
  await screen.findByText("采集仍活动或正在提交，禁止退役");
  expect(screen.queryByRole("checkbox")).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "维护采集记录" }));
  await screen.findByText("采集维护预览缺失，请重新核对");
  expect(screen.queryByRole("checkbox")).toBeNull();
  expect(trade).not.toHaveBeenCalled();
});
