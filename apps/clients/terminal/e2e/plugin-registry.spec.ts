import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";

test("Terminal registry drives settings and workspace navigation", async ({ page: workbench }) => {
  await workbench.goto("/");
  const settings = await openSettingsWindow(workbench);
  await settings.getByRole("button", { name: "插件", exact: true }).click();
  await expect(settings.getByText("已注册 · 按需加载", { exact: true })).toHaveCount(6);
  const page = await closeSettingsWindow(settings);
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByRole("heading", { name: "均线回测", exact: true })).toBeVisible();
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  await expect(page.getByRole("heading", { name: "CTP 交易", exact: true })).toBeVisible();
});
