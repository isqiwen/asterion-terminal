import { render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";
import { defaultForm } from "./research-lab";
import { RunScope } from "./RunScope";

registerTerminalFeatures([feature]);

const scope = (symbols: string) => (
  <RunScope
    context={{} as never}
    form={{ ...defaultForm(), symbols }}
    change={vi.fn()}
    catalog={{} as never}
    begin="YYYY-MM-DD"
    end="YYYY-MM-DD"
  />
);

test("the settings line asks for symbols first, then names what is chosen", () => {
  const { container, rerender } = render(scope(""));
  expect(screen.getByRole("button", { name: "设置回测条件" })).toBeTruthy();
  expect(container.textContent).not.toContain("滑点");
  rerender(scope("SHFE/rb"));
  expect(screen.getByRole("button", { name: "修改回测条件" })).toBeTruthy();
  expect(container.textContent).toContain("螺纹钢 · 全部区间 · 日线");
});
