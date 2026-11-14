import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import { feature } from "./feature";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { LabStatus, ModuleList, type ModuleItem } from "./LabParts";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
registerTerminalFeatures([feature]);

test("without a Warehouse the header reads no research data and says it is not set", () => {
  const context = { snapshot: {}, navigate: vi.fn() } as unknown as TerminalContext;
  render(<LabStatus context={context} modules={null} refresh={() => {}} />);
  expect(screen.getByRole("button", { name: /研究数据 · 未设置/ })).toBeTruthy();
  expect(lab).not.toHaveBeenCalled();
});

test("modules group by file, fold, search and filter by input, with each one's latest figure", () => {
  const items: ModuleItem[] = [
    {
      id: "price.momentum",
      name: "momentum",
      module: "price",
      description: "Return",
      inputs: ["close"],
    },
    {
      id: "price.reversal",
      name: "reversal",
      module: "price",
      description: "Negated return",
      inputs: ["close"],
      figure: { text: "0.120", title: "IC" },
    },
    {
      id: "positions.interest",
      name: "interest",
      module: "positions",
      description: "Open interest change",
      inputs: ["open_interest"],
    },
  ];
  const choose = vi.fn();
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <ModuleList folder="factors" items={items} chosen="" choose={choose} />
    </WorkspaceDraftScope>,
  );
  const price = screen.getByRole("region", { name: "price" });
  expect(within(price).getByRole("button", { name: /reversal/ }).textContent).toContain("0.120");
  // A folded group hides its members until opened again.
  fireEvent.click(within(price).getByRole("button", { name: /^price/ }));
  expect(within(price).queryByRole("button", { name: /momentum/ })).toBeNull();
  fireEvent.click(within(price).getByRole("button", { name: /^price/ }));
  fireEvent.change(screen.getByLabelText("搜索因子"), { target: { value: "negated" } });
  expect(screen.queryByRole("button", { name: /momentum/ })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: /reversal/ }));
  expect(choose).toHaveBeenCalledWith(items[1]);
  fireEvent.change(screen.getByLabelText("搜索因子"), { target: { value: "" } });
  fireEvent.change(screen.getByLabelText("输入数据"), { target: { value: "open_interest" } });
  expect(screen.queryByRole("region", { name: "price" })).toBeNull();
  expect(screen.getByRole("button", { name: /interest/ })).toBeTruthy();
});
