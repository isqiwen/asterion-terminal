import { fireEvent, render, screen } from "@testing-library/react";
import { useState } from "react";
import { beforeEach, expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";
import { defaultForm, readConditions, saveConditions, savedForm, scopeOf } from "./research-lab";
import { RunScope, useRunConditions } from "./RunScope";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn(() => new Promise(() => {})) }));
registerTerminalFeatures([feature]);
beforeEach(() => localStorage.clear());

function Page() {
  const [form, setForm] = useState(savedForm);
  const conditions = useRunConditions(setForm);
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
const line = (container: HTMLElement) => container.querySelector("p")!.textContent;

test("without settings the line only offers to set them, and needs a symbol", () => {
  const { container } = render(<Page />);
  expect(container.textContent).not.toContain("滑点");
  fireEvent.click(screen.getByRole("button", { name: "设置回测条件" }));
  expect((screen.getByRole("button", { name: "完成" }) as HTMLButtonElement).disabled).toBe(true);
  fireEvent.change(screen.getByPlaceholderText("选择品种或合约…"), {
    target: { value: "SHFE/rb," },
  });
  fireEvent.click(screen.getByRole("button", { name: "完成" }));
  expect(readConditions().active).toBe("螺纹钢 · 日线");
  expect(line(container)).toContain("螺纹钢 · 全部区间 · 日线");
});

test("switching to another set changes the settings and is remembered", () => {
  seed();
  const { container } = render(<Page />);
  expect(line(container)).toContain("滑点 1 跳");
  fireEvent.change(screen.getByLabelText("回测条件"), { target: { value: "高滑点" } });
  expect(line(container)).toContain("滑点 3 跳");
  expect(savedForm().slippage).toBe("3");
});

test("an edit applies only when done, and a new set starts from the current one", () => {
  seed();
  const { container } = render(<Page />);
  fireEvent.click(screen.getByRole("button", { name: "修改回测条件" }));
  fireEvent.change(screen.getByLabelText("滑点（跳）"), { target: { value: "9" } });
  fireEvent.click(screen.getByRole("button", { name: "取消" }));
  expect(line(container)).toContain("滑点 1 跳");

  fireEvent.change(screen.getByLabelText("回测条件"), { target: { value: "" } });
  fireEvent.change(screen.getByLabelText("名称"), { target: { value: "黑色" } });
  fireEvent.change(screen.getByLabelText("滑点（跳）"), { target: { value: "2" } });
  fireEvent.click(screen.getByRole("button", { name: "完成" }));
  expect(readConditions().items.map(item => item.name)).toEqual(["黑色", "高滑点", "黑色 2"]);
  expect(readConditions().active).toBe("黑色 2");
  expect(line(container)).toContain("滑点 2 跳");
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
