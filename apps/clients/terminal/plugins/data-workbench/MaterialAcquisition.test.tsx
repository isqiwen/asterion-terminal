import { fireEvent, render, screen, waitFor, act } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import type { MaterialSource, MaterialRead, TaskRecord, Snapshot } from "../../src/bridge/client";
import { MaterialAcquisition } from "./MaterialAcquisition";
import { plugin } from "./plugin";
import type { QueryCommand } from "../contract";
import type { CommandCall } from "../../src/bridge/commands";
registerTerminalPlugins([plugin]);
const source: MaterialSource = {
  id: "fixture-material",
  name: "Synthetic source",
  usage: "Isolated fixtures only",
  plugin_id: "fixture.material",
  provider_artifact: "a".repeat(64),
  max_bytes: 4096,
  credential_required: false,
  no_additional_fee: true,
};
const request = {
  id: "one",
  source_id: source.id,
  resource_id: "announcement",
  use_scope: "research",
  provider_artifact: source.provider_artifact,
  max_requests: 1,
  max_bytes: 4096,
};
const read: MaterialRead = {
  receipt: {
    reference: {
      node: "stable-node",
      service: "data-one",
      domain: "data",
      kind: "material_acquisition",
      id: "one",
      version: "b".repeat(64),
    },
    content: {
      node: "stable-node",
      service: "data-one",
      domain: "data",
      kind: "material_content",
      id: "c".repeat(64),
      version: "c".repeat(64),
    },
    request,
    media_type: "text/plain",
    bytes: 24,
    acquired_at_ms: "1790000000000",
    source_published_at_ms: null,
  },
  reading: { state: "readable", readable_bytes: 24 },
  text: "工厂甲计划检修",
};
const task: TaskRecord = {
  id: "one",
  kind: "material_acquisition",
  state: "succeeded",
  attempt: 1,
  completed: 1,
  total: 1,
  error: "",
  result_digest: "d".repeat(64),
  trading_day: "",
  instrument: "announcement",
  source_name: source.id,
  submission_sequence: 1,
  submitted_at_ms: 1,
  updated_at_ms: 2,
  material_request: request,
};
function context() {
  const snapshot = readySnapshot();
  return {
    snapshot,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
    trade: vi.fn(async () => {}),
    query: vi.fn(async (...args: CommandCall<QueryCommand>): Promise<Snapshot> => ({
      ...snapshot,
      ...(args[0] === "data.material.sources"
        ? { material_sources: [source] }
        : args[0] === "data.material.task"
          ? { material_task: task }
          : { material_read: read }),
    })),
  };
}
test("requires reviewed source, scope and finite budget then separates acquisition from semantic publication", async () => {
  const ctx = context();
  render(<MaterialAcquisition context={ctx} />);
  fireEvent.click(screen.getByRole("button", { name: "刷新材料来源" }));
  await screen.findByText("Isolated fixtures only");
  fireEvent.change(screen.getByLabelText("材料操作 ID"), { target: { value: "one" } });
  fireEvent.change(screen.getByLabelText("来源资源标识"), { target: { value: "announcement" } });
  fireEvent.change(screen.getByLabelText("本次使用范围"), { target: { value: "research" } });
  const submit = screen.getByRole("button", { name: "接纳材料取得任务" });
  expect((submit as HTMLButtonElement).disabled).toBe(true);
  fireEvent.click(screen.getByRole("checkbox"));
  fireEvent.click(submit);
  await waitFor(() => expect(ctx.trade).toHaveBeenCalledWith("data.material.submit", request));
  await waitFor(() => expect(ctx.query).toHaveBeenCalledWith("data.material.task", { id: "one" }));
  fireEvent.click(screen.getByRole("button", { name: "读取固定材料" }));
  await screen.findByDisplayValue("工厂甲计划检修");
  expect(screen.getByText("材料取得成功，尚未进行语义核验或情报发布。")).toBeTruthy();
  expect(screen.getByText(/来源发布时间/).textContent).toContain("未知");
  expect(ctx.trade).toHaveBeenCalledTimes(1);
});
test("discards an old material reply after selecting another Data owner", async () => {
  const ctx = context();
  let resolve!: (value: Snapshot) => void;
  ctx.query.mockImplementation(
    async () =>
      new Promise<Snapshot>(r => {
        resolve = r;
      }),
  );
  const view = render(<MaterialAcquisition context={ctx} />);
  fireEvent.change(screen.getByLabelText("材料操作 ID"), { target: { value: "one" } });
  fireEvent.click(screen.getByRole("button", { name: "读取固定材料" }));
  const next = {
    ...ctx,
    snapshot: {
      ...ctx.snapshot,
      data: { ...ctx.snapshot.data!, connection_id: "another-connection", service: "other-data" },
    },
  };
  view.rerender(<MaterialAcquisition context={next} />);
  await act(async () => resolve({ ...ctx.snapshot, material_read: read }));
  expect(screen.queryByDisplayValue("工厂甲计划检修")).toBeNull();
});
test("declared paid or credential sources remain unavailable rather than becoming a download", async () => {
  const ctx = context();
  ctx.query.mockResolvedValue({
    ...ctx.snapshot,
    material_sources: [{ ...source, no_additional_fee: false }],
  });
  render(<MaterialAcquisition context={ctx} />);
  fireEvent.click(screen.getByRole("button", { name: "刷新材料来源" }));
  await screen.findByText("现有来源需要凭据或费用准入，当前取得路径不可用。");
  expect(
    (screen.getByRole("button", { name: "接纳材料取得任务" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(ctx.trade).not.toHaveBeenCalled();
});
