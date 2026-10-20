import { openView } from "./navigation-helper";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";

test("Terminal registry drives settings and workspace navigation", async ({ page: workbench }) => {
  await workbench.goto("/");
  const settings = await openSettingsWindow(workbench);
  await settings.getByRole("button", { name: "插件", exact: true }).click();
  await expect(settings.getByText("内置 · 随应用发布", { exact: true })).toHaveCount(4);
  const page = await closeSettingsWindow(settings);
  await openView(page, "backtest");
  await expect(page.getByRole("heading", { name: "策略回测", exact: true })).toBeVisible();
  await openView(page, "trading");
  await expect(page.getByRole("heading", { name: "CTP 交易", exact: true })).toBeVisible();
});
