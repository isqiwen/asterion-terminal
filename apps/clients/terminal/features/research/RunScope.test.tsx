import { act, fireEvent, render, screen } from "@testing-library/react";
import { useState } from "react";
import { beforeEach, expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";
import {
  defaultForm,
  readConditions,
  saveConditions,
  savedForm,
  scopeOf,
  type ScopeFields,
} from "./research-lab";
import { RunScope, useRunConditions } from "./RunScope";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn(() => new Promise(() => {})) }));
registerTerminalFeatures([feature]);
beforeEach(() => localStorage.clear());

function Page({ onAdopt }: { onAdopt?: (adopt: (fields: ScopeFields) => ScopeFields) => void }) {
  const [form, setForm] = useState(savedForm);
  const conditions = useRunConditions(setForm);
  onAdopt?.(conditions.adopt);
  return (
    <RunScope
      context={{ snapshot: { research_directory: "/research" } } as never}
      form={form}
      begin="YYYY-MM-DD"
      end="YYYY-MM-DD"
      conditions={conditions}
    />
  );
}
const seed = () =>
  saveConditions({
    active: "黑色",
    items: [
      { name: "黑色", fields: scopeOf({ ...defaultForm(), symbols: "SHFE/rb" }) },
      { name: "高滑点", fields: scopeOf({ ...defaultForm(), symbols: "SHFE/rb", slippage: "3" }) },
    ],
  });
const line = (container: HTMLElement) => container.querySelector("p")?.textContent ?? "";

test("ad-hoc setup persists the working draft without creating a saved preset", () => {
  const { container } = render(<Page />);
  expect(container.textContent).not.toContain("滑点");
  expect(screen.getByRole("button", { name: "快速选择品种" }).textContent).toBe("选择品种");

  fireEvent.click(screen.getByRole("button", { name: "设置回测条件" }));
  expect((screen.getByRole("button", { name: "完成" }) as HTMLButtonElement).disabled).toBe(true);
  fireEvent.change(screen.getByPlaceholderText("选择品种或合约…"), {
    target: { value: "SHFE/rb," },
  });
  fireEvent.click(screen.getByRole("button", { name: "完成" }));

  // Working draft is active and remembered, while saved presets remain empty.
  expect(readConditions().items).toEqual([]);
  expect(savedForm().symbols).toBe("SHFE/rb");
  expect(screen.getByRole("button", { name: "快速选择品种" }).textContent).toBe("螺纹钢");
  expect((screen.getByLabelText("快速切换周期") as HTMLSelectElement).value).toBe("0");
  expect(line(container)).toContain("全部区间");
  expect(line(container)).toContain("滑点 1 跳");
  expect(screen.getByRole("button", { name: "存为预设" })).toBeTruthy();
});

test("switching presets, inline quick edits, revert, and explicit save/update work cleanly", () => {
  seed();
  const { container } = render(<Page />);
  expect(line(container)).toContain("滑点 1 跳");

  // Switching preset loads its fields immediately.
  fireEvent.change(screen.getByLabelText("回测条件"), { target: { value: "高滑点" } });
  expect(line(container)).toContain("滑点 3 跳");
  expect(savedForm().slippage).toBe("3");
  expect(screen.queryByText("已修改")).toBeNull();

  // Inline quick switch edits the draft only, showing dirty state and revert.
  fireEvent.change(screen.getByLabelText("快速切换周期"), { target: { value: "5" } });
  expect(savedForm().minutes).toBe("5");
  expect(screen.getByText("已修改")).toBeTruthy();
  expect(readConditions().items.find(item => item.name === "高滑点")?.fields.minutes).toBe("0");
  fireEvent.click(screen.getByRole("button", { name: "恢复" }));
  expect(savedForm().minutes).toBe("0");
  expect(screen.queryByText("已修改")).toBeNull();

  // Dialog cancel discards edits; applying marks dirty and allows explicit update or save-as.
  fireEvent.click(screen.getByRole("button", { name: "修改回测条件" }));
  fireEvent.change(screen.getByLabelText("滑点（跳）"), { target: { value: "9" } });
  fireEvent.click(screen.getByRole("button", { name: "取消" }));
  expect(line(container)).toContain("滑点 3 跳");

  fireEvent.click(screen.getByRole("button", { name: "修改回测条件" }));
  fireEvent.change(screen.getByLabelText("滑点（跳）"), { target: { value: "2" } });
  fireEvent.click(screen.getByRole("button", { name: "完成" }));
  expect(line(container)).toContain("滑点 2 跳");
  expect(screen.getByText("已修改")).toBeTruthy();

  // Explicit save with a colliding non-active name creates a unique preset instead of clobbering.
  fireEvent.click(screen.getByRole("button", { name: "存为预设" }));
  fireEvent.change(screen.getByLabelText("名称"), { target: { value: "黑色" } });
  fireEvent.click(screen.getByRole("button", { name: "保存" }));
  expect(readConditions().items.map(item => item.name)).toEqual(["黑色", "高滑点", "黑色 2"]);
  expect(readConditions().active).toBe("黑色 2");
  expect(screen.queryByText("已修改")).toBeNull();

  // Updating the currently active preset overwrites it in place.
  fireEvent.change(screen.getByLabelText("快速切换周期"), { target: { value: "15" } });
  fireEvent.click(screen.getByRole("button", { name: "存为预设" }));
  fireEvent.click(screen.getByRole("button", { name: "更新预设" }));
  expect(readConditions().items.map(item => item.name)).toEqual(["黑色", "高滑点", "黑色 2"]);
  expect(readConditions().items[2]!.fields.minutes).toBe("15");
  expect(screen.queryByText("已修改")).toBeNull();
});

test("adopting settings updates only the draft unless an identical preset already exists", () => {
  seed();
  let adopt!: (fields: ScopeFields) => ScopeFields;
  render(
    <Page
      onAdopt={fn => {
        adopt = fn;
      }}
    />,
  );
  const adhoc = scopeOf({ ...defaultForm(), symbols: "DCE/i", slippage: "5" });

  // Adopting novel settings does not create a ghost preset and displays as ad-hoc.
  act(() => {
    adopt(adhoc);
  });
  expect(readConditions().items.map(item => item.name)).toEqual(["黑色", "高滑点"]);
  expect(readConditions().active).toBe("");
  expect((screen.getByLabelText("回测条件") as HTMLSelectElement).value).toBe("");
  expect(screen.getByRole("option", { name: "临时条件" })).toBeTruthy();
  expect(savedForm().symbols).toBe("DCE/i");
  expect(savedForm().slippage).toBe("5");

  // Adopting settings identical to an existing preset selects that preset.
  act(() => {
    adopt(scopeOf({ ...defaultForm(), symbols: "SHFE/rb", slippage: "3" }));
  });
  expect(readConditions().active).toBe("高滑点");
  expect(readConditions().items.map(item => item.name)).toEqual(["黑色", "高滑点"]);
});

test("deleting the set in use moves to the next one", () => {
  seed();
  const { container } = render(<Page />);
  fireEvent.click(screen.getByRole("button", { name: "修改回测条件" }));
  fireEvent.click(screen.getByRole("button", { name: "删除 黑色" }));
  fireEvent.click(screen.getByRole("button", { name: "确认删除 黑色" }));
  expect(readConditions().items.map(item => item.name)).toEqual(["高滑点"]);
  expect(line(container)).toContain("滑点 3 跳");
});
