import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { EditorView } from "codemirror";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import { RunsSchema, SourceSchema, StrategiesSchema } from "../../src/generated/asterion/lab_pb";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { StrategyLab } from "./StrategyLab";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
}));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, string>) =>
    values ? `${key} ${Object.values(values).join(" ")}` : key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

const editor = () => EditorView.findFromDOM(document.querySelector(".cm-editor") as HTMLElement)!;

test("an unsaved edit is saved before the run reads it, and the run opens its report", async () => {
  const files: Record<string, string> = { "strategies/once.py": "LOTS = 1\n" };
  const order: string[] = [];
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, { strategies: [{ id: "once.Once" }] }) as never;
      case "listRuns":
        return create(RunsSchema, {}) as never;
      case "readSource":
        return create(SourceSchema, { content: files[operation.value.path!] }) as never;
      case "saveSource":
        files[operation.value.source!.path!] = operation.value.content!;
        order.push(`saved ${operation.value.content}`);
        return {} as never;
      case "startBacktest":
        order.push(`ran with ${files["strategies/once.py"]}`);
        return {} as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: /Once/ }));
  await waitFor(() => expect(editor().state.doc.toString()).toBe("LOTS = 1\n"));
  editor().dispatch({ changes: { from: 7, to: 8, insert: "2" } });
  fireEvent.change(screen.getByPlaceholderText("SHFE/rb, SHFE/au/2026-12"), {
    target: { value: "SHFE/rb" },
  });
  fireEvent.click(screen.getByRole("button", { name: "运行" }));

  // The edit was saved first and the run read it; its report opens in the runs view.
  await screen.findByText("已保存 {files} strategies/once.py");
  expect(order).toEqual(["saved LOTS = 2\n", "ran with LOTS = 2\n"]);
  expect(context.navigate).toHaveBeenCalledWith("workspace.lab-runs", {
    params: { run: expect.stringMatching(/^once-/) },
  });
  // The code stays open for the next edit.
  expect(editor().state.doc.toString()).toBe("LOTS = 2\n");
});

test("the run configuration folds away to give the result the width", async () => {
  vi.mocked(lab).mockImplementation(async operation =>
    operation?.case === "listStrategies"
      ? (create(StrategiesSchema, { strategies: [] }) as never)
      : operation?.case === "listRuns"
        ? (create(RunsSchema, {}) as never)
        : (new Promise(() => {}) as never),
  );
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  const { container } = render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "收起配置" }));
  expect(container.querySelector(".lab-research")?.hasAttribute("data-folded")).toBe(true);
  expect(screen.getAllByText("选择策略")).toHaveLength(1);
  fireEvent.click(screen.getByRole("button", { name: "展开配置" }));
  expect(screen.getAllByText("选择策略")).toHaveLength(2);
});

test("a folded group says what is set, and a recent run opens its report", async () => {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, { strategies: [{ id: "once.Once" }] }) as never;
      case "listRuns":
        return create(RunsSchema, {
          runs: [
            { id: "once-2", strategy: "once.Once", label: "快线", metrics: { total_return: 0.1 } },
            { id: "other-1", strategy: "other.Other" },
          ],
        }) as never;
      case "readSource":
        return create(SourceSchema, { content: "" }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: /Once/ }));
  const funds = screen.getByText("资金与风控").closest("details")!;
  expect(funds.open).toBe(false);
  expect(funds.querySelector("summary")!.textContent).toContain("资金 {deposit}");
  const recent = await screen.findByRole("region", { name: "最近运行" });
  expect(recent.textContent).not.toContain("other-1");
  fireEvent.click(screen.getByRole("button", { name: /快线/ }));
  expect(context.navigate).toHaveBeenCalledWith("workspace.lab-runs", {
    params: { run: "once-2" },
  });
});
