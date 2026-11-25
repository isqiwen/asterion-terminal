import { CompletionContext } from "@codemirror/autocomplete";
import { diagnosticCount } from "@codemirror/lint";
import { EditorState } from "@codemirror/state";
import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { EditorView } from "codemirror";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import { SourceSchema } from "../../src/generated/asterion/lab_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { LabCentre, useSource } from "./LabParts";
import { feature } from "./feature";
import { SourceEditor, strategyCompletion, tracebackDiagnostics } from "./SourceEditor";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
registerTerminalFeatures([feature]);

function Workspace() {
  const source = useSource("/research");
  return (
    <>
      <button onClick={() => source.open("strategies/a.py")}>A</button>
      <button onClick={() => source.open("strategies/b.py")}>B</button>
      <LabCentre source={source} repository="/research" changed={() => {}} />
      <output>{JSON.stringify(source.drafts)}</output>
    </>
  );
}

test("manual save acknowledges its text while newer editor changes survive switching files", async () => {
  let finish!: () => void;
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case === "readSource")
      return create(SourceSchema, { content: "original = 0" }) as never;
    if (operation.case === "saveSource")
      await new Promise<void>(resolve => {
        finish = resolve;
      });
    return {} as never;
  });
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="research">
      <Workspace />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(screen.getByRole("button", { name: "A" }));
  await waitFor(() => expect(document.querySelector(".cm-editor")).not.toBeNull());
  const editor = EditorView.findFromDOM(document.querySelector(".cm-editor") as HTMLElement)!;
  const edit = (text: string) =>
    act(() => editor.dispatch({ changes: { from: 0, to: editor.state.doc.length, insert: text } }));
  edit("saved = 1");
  fireEvent.click(screen.getByRole("button", { name: "保存" }));
  edit("original = 0");
  await act(async () => {
    finish();
  });
  expect((screen.getByRole("button", { name: "保存" }) as HTMLButtonElement).disabled).toBe(false);
  fireEvent.click(screen.getByRole("button", { name: "B" }));
  expect(screen.getByRole("status").textContent).toContain('"strategies/a.py":"original = 0"');
});

test("choosing a symbol puts the caret on its definition", async () => {
  const content = "class First:\n    pass\n\nclass Second:\n    pass\n";
  vi.mocked(lab).mockResolvedValue(create(SourceSchema, { content }) as never);
  const editor = (symbol: string) => (
    <SourceEditor
      repository="/research"
      path="strategies/a.py"
      draft={undefined}
      symbol={symbol}
      edited={() => {}}
      saved={() => {}}
      changed={() => {}}
    />
  );
  const { rerender } = render(editor("Second"));
  await waitFor(() => expect(document.querySelector(".cm-editor")).not.toBeNull());
  const view = EditorView.findFromDOM(document.querySelector(".cm-editor") as HTMLElement)!;
  expect(view.state.selection.main.head).toBe(content.indexOf("class Second"));
  rerender(editor("First"));
  expect(view.state.selection.main.head).toBe(0);
});

test("Tab leaves a read-only source without intercepting keyboard navigation", async () => {
  vi.mocked(lab).mockResolvedValue(
    create(SourceSchema, { content: "builtin = 1", readOnly: true }) as never,
  );
  render(
    <SourceEditor
      repository="/research"
      path="strategies/builtin.py"
      draft={undefined}
      edited={vi.fn()}
      saved={vi.fn()}
      changed={vi.fn()}
    />,
  );
  const editor = await screen.findByRole("textbox", { name: "strategies/builtin.py" });
  expect(fireEvent.keyDown(editor, { key: "Tab" })).toBe(true);
  expect(editor.textContent).toBe("builtin = 1");
});

test("strategy completions suggest Context/Bar members and traceback marks the failing line", async () => {
  const query = (doc: string) =>
    strategyCompletion(new CompletionContext(EditorState.create({ doc }), doc.length, true));
  expect(query("ctx.")?.options.map(item => item.label)).toContain("history");
  expect(query("ctx.")?.options.map(item => item.label)).toContain("factor");
  expect(query("bar.")?.options.map(item => item.label)).toContain("open_interest");
  expect(query("bar.")?.options.map(item => item.label)).toContain("roll_yield");
  expect(query('ctx.history(s, "cl')?.options.map(item => item.label)).toContain("close");
  expect(query('ctx.history(s, "ro')?.options.map(item => item.label)).toContain("roll_yield");
  expect(query("sci")?.options.map(item => item.label)).toContain("scipy");

  const doc = EditorState.create({
    doc: "from asterion_strategy import Strategy\n    bad = (\n",
  }).doc;
  const diagnostics = tracebackDiagnostics(
    doc,
    "strategies/broken.py",
    "Traceback (most recent call last):\n  File \"/tmp/strategies/broken.py\", line 2\n    bad = (\nSyntaxError: '(' was never closed",
  );
  expect(diagnostics).toEqual([
    {
      from: doc.line(2).from + 4,
      to: doc.line(2).to,
      severity: "error",
      message: "SyntaxError: '(' was never closed",
    },
  ]);

  vi.mocked(lab).mockResolvedValue(
    create(SourceSchema, {
      content: "from asterion_strategy import Strategy\n    bad = (\n",
      readOnly: false,
    }) as never,
  );
  render(
    <SourceEditor
      repository="/research"
      path="strategies/broken.py"
      draft={undefined}
      traceback={
        'Traceback (most recent call last):\n  File "/tmp/strategies/broken.py", line 2\nSyntaxError: invalid syntax'
      }
      edited={vi.fn()}
      saved={vi.fn()}
      changed={vi.fn()}
    />,
  );
  await waitFor(() => expect(document.querySelector(".cm-editor")).not.toBeNull());
  const view = EditorView.findFromDOM(document.querySelector(".cm-editor") as HTMLElement)!;
  await waitFor(() => expect(diagnosticCount(view.state)).toBe(1));
  act(() => view.dispatch({ changes: { from: view.state.doc.length, insert: "# edit" } }));
  await waitFor(() => expect(diagnosticCount(view.state)).toBe(0));
});

test("window focus reloads clean external edits without creating a draft", async () => {
  let disk = "x = 1\n";
  const edited = vi.fn();
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case === "readSource")
      return create(SourceSchema, { content: disk, readOnly: false }) as never;
    return {} as never;
  });
  render(
    <SourceEditor
      repository="/research"
      path="strategies/external.py"
      draft={undefined}
      edited={edited}
      saved={vi.fn()}
      changed={vi.fn()}
    />,
  );
  await waitFor(() => expect(document.querySelector(".cm-editor")).not.toBeNull());
  const view = EditorView.findFromDOM(document.querySelector(".cm-editor") as HTMLElement)!;
  expect(view.state.doc.toString()).toBe("x = 1\n");
  disk = "x = 2\n";
  act(() => {
    window.dispatchEvent(new Event("focus"));
  });
  await waitFor(() => expect(view.state.doc.toString()).toBe("x = 2\n"));
  expect(edited).not.toHaveBeenCalled();
  expect((screen.getByRole("button", { name: "保存" }) as HTMLButtonElement).disabled).toBe(true);
});

test("source header offers duplicate-and-edit for read-only modules and two-click delete for custom modules only", async () => {
  const onDelete = vi.fn();
  const onDuplicate = vi.fn();
  vi.mocked(lab).mockResolvedValueOnce(
    create(SourceSchema, { content: "builtin = 1", readOnly: true }) as never,
  );
  const { rerender } = render(
    <SourceEditor
      repository="/research"
      path="strategies/builtin.py"
      draft={undefined}
      edited={vi.fn()}
      saved={vi.fn()}
      changed={vi.fn()}
      onDuplicate={onDuplicate}
      onDelete={onDelete}
    />,
  );
  await screen.findByText("只读");
  expect(screen.queryByRole("button", { name: /删除/ })).toBeNull();
  fireEvent.click(await screen.findByRole("button", { name: "复制并编辑" }));
  expect(onDuplicate).toHaveBeenCalledWith("builtin = 1");

  vi.mocked(lab).mockResolvedValueOnce(
    create(SourceSchema, { content: "custom = 2", readOnly: false }) as never,
  );
  rerender(
    <SourceEditor
      key="custom"
      repository="/research"
      path="strategies/custom.py"
      draft={undefined}
      edited={vi.fn()}
      saved={vi.fn()}
      changed={vi.fn()}
      onDuplicate={onDuplicate}
      onDelete={onDelete}
    />,
  );
  const del = await screen.findByRole("button", { name: "删除 strategies/custom.py" });
  expect(screen.queryByRole("button", { name: "复制并编辑" })).toBeNull();
  fireEvent.click(del);
  expect(onDelete).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "确认删除 strategies/custom.py" }));
  expect(onDelete).toHaveBeenCalledTimes(1);
});
