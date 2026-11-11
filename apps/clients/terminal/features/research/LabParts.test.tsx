import { render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import { feature } from "./feature";
import { LabHeader } from "./LabParts";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
registerTerminalFeatures([feature]);

test("without a Warehouse the header reads no research data and says it is not set", () => {
  const context = { snapshot: {}, navigate: vi.fn() } as unknown as TerminalContext;
  render(<LabHeader context={context} modules={null} refresh={() => {}} />);
  expect(screen.getByRole("button", { name: /研究数据 · 未设置/ })).toBeTruthy();
  expect(lab).not.toHaveBeenCalled();
});
