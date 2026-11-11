import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { EditorView } from "codemirror";
import { expect, test, vi } from "vitest";
import { lab, type TerminalContext } from "../contract";
import { RunsSchema, SourceSchema, StrategiesSchema } from "../../src/generated/asterion/lab_pb";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { StrategyLab } from "./StrategyLab";

vi.mock("../contract", async original => ({
  ...(await original<typeof import("../contract")>()),
  lab: vi.fn(),
  translate: (_namespace: string, key: string, values?: Record<string, string>) =>
    values ? `${key} ${Object.values(values).join(" ")}` : key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

const editor = () => EditorView.findFromDOM(document.querySelector(".cm-editor") as HTMLElement)!;

test("an unsaved edit survives the result and is saved before the run reads it", async () => {
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

  // The run shows its result; the edit was saved first and the run read it.
  await screen.findByText("已保存 {files} strategies/once.py");
  expect(order).toEqual(["saved LOTS = 2\n", "ran with LOTS = 2\n"]);
  await waitFor(() => expect(document.querySelector(".cm-editor")).toBeNull());
  fireEvent.click(screen.getByRole("button", { name: "代码" }));
  await waitFor(() => expect(editor().state.doc.toString()).toBe("LOTS = 2\n"));

  // An edit left unsaved is still there after the result and back.
  editor().dispatch({ changes: { from: 7, to: 8, insert: "3" } });
  fireEvent.click(screen.getByRole("button", { name: "结果" }));
  fireEvent.click(screen.getByRole("button", { name: "代码" }));
  await waitFor(() => expect(editor().state.doc.toString()).toBe("LOTS = 3\n"));
  expect(screen.getByRole("button", { name: "保存" }).hasAttribute("disabled")).toBe(false);
  expect(files["strategies/once.py"]).toBe("LOTS = 2\n");
});
