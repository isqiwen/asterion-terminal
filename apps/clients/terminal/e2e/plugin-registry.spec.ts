import { openView } from "./navigation-helper";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";

test("Terminal registry drives settings and workspace navigation", async ({ page: workbench }) => {
  await workbench.goto("/");
  const settings = await openSettingsWindow(workbench);
  await settings.getByRole("button", { name: "插件", exact: true }).click();
  await expect(
    settings.getByRole("heading", { name: "服务启用的插件", exact: true }),
  ).toBeVisible();
  const page = await closeSettingsWindow(settings);
  await openView(page, "backtest");
  await expect(page.getByRole("button", { name: "新建回测", exact: true })).toBeVisible();
  await openView(page, "trading");
  await expect(page.getByRole("heading", { name: "CTP 交易", exact: true })).toBeVisible();
});
