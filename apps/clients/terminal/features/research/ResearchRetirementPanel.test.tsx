import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { ResearchRetirement, TaskOwnerRef } from "../../src/bridge/client";
import { feature } from "./feature";
import { ResearchRetirementPanel } from "./ResearchRetirementPanel";
registerTerminalFeatures([feature]);
const snapshot = readySnapshot();
const owner: TaskOwnerRef = {
  kind: "research",
  node_id: "local",
  service: "research-one",
  connection_id: "connection",
};
const available: ResearchRetirement = {
  id: "run",
  node: "stable-node",
  service: "research-one",
  version: "a".repeat(64),
  state: "available",
  active: false,
  admission_busy: false,
  consumers: [],
  source_release_confirmed: false,
  release_error: null,
  task: {
    id: "run",
    kind: "backtest",
    state: "cancelled",
    attempt: 0,
    completed: 0,
    total: 3,
    error: "",
    result_digest: "",
    trading_day: "",
    instrument: "SHFE/rb",
    source_name: "fixture",
    submission_sequence: 1,
    submitted_at_ms: 1,
    updated_at_ms: 1,
  },
};
const reply = (value: ResearchRetirement) => ({ ...snapshot, task_retirement: value });
test("activity and accepted derivations block confirmation without any write", async () => {
  const query = vi.fn(async () =>
    reply({ ...available, state: "blocked", active: true, consumers: ["derived"] }),
  );
  const trade = vi.fn();
  render(
    <ResearchRetirementPanel id="run" owner={owner} busy={false} query={query} trade={trade} />,
  );
  expect(query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "维护研究记录" }));
  await screen.findByText("存在活动或派生引用，禁止退役");
  expect(screen.getByText("derived")).toBeTruthy();
  expect(screen.queryByRole("checkbox")).toBeNull();
  expect(trade).not.toHaveBeenCalled();
});
test("explicit confirmation executes against the exact owner and rechecks real release", async () => {
  const query = vi
    .fn()
    .mockResolvedValueOnce(reply(available))
    .mockResolvedValue(reply({ ...available, state: "retired", source_release_confirmed: true }));
  const trade = vi.fn(async () => undefined);
  render(
    <ResearchRetirementPanel id="run" owner={owner} busy={false} query={query} trade={trade} />,
  );
  fireEvent.click(screen.getByRole("button", { name: "维护研究记录" }));
  await screen.findByText("可退役");
  expect((screen.getByRole("button", { name: /^确认退役$/ }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  fireEvent.click(screen.getByRole("checkbox", { name: "确认退役此记录，禁止重试和新增派生" }));
  fireEvent.click(screen.getByRole("button", { name: /^确认退役$/ }));
  await screen.findByText("Data 已确认解除源保留");
  expect(trade).toHaveBeenCalledWith("task.retirement.execute", { id: "run", owner });
  expect(screen.getByText(/不会删除载荷或级联删除 Data 内容/)).toBeTruthy();
});
test("offline source leaves retirement distinct from release and can replay the same identity", async () => {
  const query = vi
    .fn()
    .mockResolvedValueOnce(reply({ ...available, state: "retired" }))
    .mockResolvedValue(reply({ ...available, state: "retired", source_release_confirmed: true }));
  const trade = vi.fn(async () => undefined);
  render(
    <ResearchRetirementPanel id="run" owner={owner} busy={false} query={query} trade={trade} />,
  );
  fireEvent.click(screen.getByRole("button", { name: "维护研究记录" }));
  await screen.findByText("源保留尚未确认解除，来源内容继续受保护");
  expect(screen.queryByText("Data 已确认解除源保留")).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "重试解除源保留" }));
  await screen.findByText("Data 已确认解除源保留");
  expect(trade).toHaveBeenLastCalledWith("task.retirement.execute", { id: "run", owner });
});
