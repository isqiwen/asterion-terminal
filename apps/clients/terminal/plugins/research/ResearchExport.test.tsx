import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { ResearchExport } from "./ResearchExport";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
test("development download reports exact receipt and explains summary-only input evidence", async () => {
  Object.defineProperty(URL, "createObjectURL", {
    configurable: true,
    value: vi.fn(() => "blob:isolated-export"),
  });
  Object.defineProperty(URL, "revokeObjectURL", { configurable: true, value: vi.fn() });
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
  const query = vi.fn(async () => ({
    ...readySnapshot(),
    research_export: {
      name: "task.evidence.json",
      sha256: "a".repeat(64),
      bytes: 40,
      content: '{"version":1}',
    },
  }));
  render(<ResearchExport id="task" kind="factor" busy={false} query={query} />);
  fireEvent.click(screen.getByRole("button", { name: "导出研究证据" }));
  const status = await screen.findByRole("status");
  expect(status.textContent).toContain("研究证据下载已发起");
  expect(status.textContent).toContain("a".repeat(64));
  expect(query).toHaveBeenCalledWith("task.evidence.export", { id: "task", path: "" });
  expect(screen.getByText(/因子原始输入摘要未由结果接口提供/)).toBeTruthy();
  expect(click).toHaveBeenCalledTimes(1);
  click.mockRestore();
});
test("a failed verified read produces no success receipt or fallback download", async () => {
  const query = vi.fn(async () => {
    throw new Error("task does not exist");
  });
  render(<ResearchExport id="missing" kind="backtest" busy={false} query={query} />);
  fireEvent.click(screen.getByRole("button", { name: "导出研究证据" }));
  await screen.findByText("task does not exist");
  expect(screen.queryByRole("status")).toBeNull();
  expect(query).toHaveBeenCalledTimes(1);
});
