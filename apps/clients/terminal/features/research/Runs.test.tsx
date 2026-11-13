import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import { Period } from "../../src/generated/asterion/common_pb";
import { RunDetailSchema, RunState, RunsSchema } from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { Runs } from "./Runs";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
}));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

const run = (id: string, sharpe: number, extra: object = {}) => ({
  id,
  strategy: "ma_cross.MaCross",
  state: RunState.SUCCEEDED,
  symbols: ["SHFE/rb"],
  firstDay: "2026-01-05",
  lastDay: "2026-02-13",
  period: Period.DAY,
  metrics: { sharpe, total_return: sharpe / 10, fills: 4 },
  params: { fast: { value: { case: "integer" as const, value: 2n } } },
  ...extra,
});
function serve(runs: ReturnType<typeof run>[]) {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listRuns":
        return create(RunsSchema, { runs } as never) as never;
      case "getRun":
        return create(RunDetailSchema, {
          summary: runs.find(item => item.id === operation.value.id),
          record: JSON.stringify({
            strategy: { id: "ma_cross.MaCross", commit: "commit", changes: "" },
            params: {},
            costs: { deposit: "1000000" },
            engine: { version: "test" },
            data: {
              dataset_version: "a".repeat(64),
              symbols: ["SHFE/rb"],
              begin_day: "2026-01-05",
              end_day: "2026-02-13",
              bars: { period: "day" },
            },
          }),
        } as never) as never;
    }
    return {} as never;
  });
}
const view = (params?: Record<string, string>) =>
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-runs">
      <Runs {...({ workspaceParams: params, navigate: vi.fn() } as unknown as TerminalContext)} />
    </WorkspaceDraftScope>,
  );
const names = () =>
  within(screen.getByRole("table", { name: "回测记录" }))
    .getAllByRole("row")
    .slice(1)
    .map(row => within(row).getAllByRole("button")[0]!.textContent);

test("records say what each run ran, sort by a figure and narrow by search", async () => {
  serve([run("r1", 1, { label: "快线" }), run("r2", 2, { symbols: ["DCE/i"] })]);
  view();
  await screen.findByRole("table", { name: "回测记录" });
  expect(screen.getByRole("row", { name: /快线/ }).textContent).toContain(
    "2026-01-05 – 2026-02-13",
  );
  expect(screen.getByRole("row", { name: /快线/ }).textContent).toContain("fast=2");
  fireEvent.click(screen.getByRole("button", { name: "夏普比率" }));
  expect(names()).toEqual(["r2", "快线"]);
  fireEvent.click(screen.getByRole("button", { name: "夏普比率 ↓" }));
  expect(names()).toEqual(["快线", "r2"]);
  fireEvent.change(screen.getByLabelText("搜索名称或品种"), { target: { value: "dce" } });
  expect(names()).toEqual(["r2"]);
});

test("selected runs open side by side", async () => {
  serve([run("r1", 1), run("r2", 2), run("r3", 3)]);
  view();
  await screen.findByRole("table", { name: "回测记录" });
  const compare = screen.getByRole("button", { name: "比较所选（0）" });
  expect(compare.hasAttribute("disabled")).toBe(true);
  for (const id of ["r1", "r3"]) fireEvent.click(screen.getByLabelText(`比较 ${id}`));
  fireEvent.click(screen.getByRole("button", { name: "比较所选（2）" }));
  await screen.findByRole("columnheader", { name: "r3" });
  expect(screen.getByRole("columnheader", { name: "r1" })).toBeTruthy();
});

test("a run opened from the strategy view shows its report and takes a name", async () => {
  serve([run("r1", 1)]);
  view({ run: "r1" });
  await screen.findByRole("heading", { name: "r1" });
  fireEvent.click(screen.getByRole("button", { name: "重命名" }));
  fireEvent.change(screen.getByLabelText("名称"), { target: { value: "快线" } });
  fireEvent.click(screen.getByRole("button", { name: "保存" }));
  await waitFor(() =>
    expect(vi.mocked(lab)).toHaveBeenCalledWith({
      case: "renameRun",
      value: { id: "r1", label: "快线" },
    }),
  );
  fireEvent.click(screen.getByRole("button", { name: "← 回测记录" }));
  await screen.findByRole("table", { name: "回测记录" });
});

test("a run is deleted only on the second click", async () => {
  serve([run("r1", 1)]);
  view();
  fireEvent.click(await screen.findByRole("button", { name: "删除 r1" }));
  expect(vi.mocked(lab).mock.calls.some(([call]) => call?.case === "deleteRun")).toBe(false);
  fireEvent.click(screen.getByRole("button", { name: "确认删除 r1" }));
  await waitFor(() =>
    expect(vi.mocked(lab)).toHaveBeenCalledWith({ case: "deleteRun", value: { id: "r1" } }),
  );
});
