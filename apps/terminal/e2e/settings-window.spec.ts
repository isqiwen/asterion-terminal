import { test, expect } from "@playwright/test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";

test("settings reuses its window and preserves workbench drafts", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByLabel("CSV 文件路径").fill("unsaved-draft.csv");
  const settings = await openSettingsWindow(page);
  await settings.setViewportSize({ width: 820, height: 620 });
  await expect(settings.getByRole("heading", { name: "通用", exact: true })).toBeVisible();
  await expect(page.getByLabel("CSV 文件路径")).toHaveValue("unsaved-draft.csv");
  await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
  await expect(page.locator("html")).toHaveAttribute("lang", "en-US");
  await settings.getByRole("button", { name: "Appearance", exact: true }).click();
  await settings.getByLabel("Display Density").selectOption("comfortable");
  await settings.screenshot({ path: "apps/terminal/test-results/settings-window.png" });
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "General", exact: true })).toBeVisible();
  expect(page.context().pages()).toHaveLength(2);
  await settings.getByLabel("Language").selectOption("zh-CN");
  await page.getByRole("button", { name: "查看服务连接", exact: true }).click();
  await page.getByRole("button", { name: "连接设置", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "连接与部署", exact: true })).toBeVisible();
  expect(page.context().pages()).toHaveLength(2);
  await settings.screenshot({ path: "apps/terminal/test-results/settings-connections.png" });
  expect(await settings.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
    true,
  );
  await closeSettingsWindow(settings);
  await expect(page.getByLabel("CSV 文件路径")).toHaveValue("unsaved-draft.csv");
  const reopened = await openSettingsWindow(page);
  await expect(reopened.getByRole("heading", { name: "通用", exact: true })).toBeVisible();
  await closeSettingsWindow(reopened);
});
