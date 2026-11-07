import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";

test("settings reuses its window and preserves workbench drafts", async ({ page }) => {
  await page.goto("/");
  await openView(page, "market");
  await page.getByRole("searchbox", { name: "搜索市场合约" }).fill("rb17");
  const settings = await openSettingsWindow(page);
  await settings.setViewportSize({ width: 820, height: 540 });
  await expect(settings.getByRole("heading", { name: "偏好设置", exact: true })).toBeVisible();
  await expect(page.getByRole("searchbox", { name: "搜索市场合约" })).toHaveValue("rb17");
  await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
  await expect(page.locator("html")).toHaveAttribute("lang", "en-US");
  await settings.getByLabel("Display Density").selectOption("comfortable");
  await settings.screenshot({ path: "apps/clients/terminal/test-results/settings-window.png" });
  await settings.getByRole("button", { name: "About", exact: true }).click();
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "About", exact: true })).toBeVisible();
  await settings.keyboard.press("Control+,");
  await expect(settings.getByRole("heading", { name: "About", exact: true })).toBeVisible();
  await page.getByRole("searchbox", { name: "Search market contracts" }).fill("rb19");
  await settings.getByRole("button", { name: "Preferences", exact: true }).click();
  expect(page.context().pages()).toHaveLength(2);
  await settings.getByLabel("Language").selectOption("zh-CN");
  await settings.getByRole("button", { name: "交易服务", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "交易服务", exact: true })).toBeVisible();
  expect(page.context().pages()).toHaveLength(2);
  await settings.screenshot({
    path: "apps/clients/terminal/test-results/settings-connections.png",
  });
  expect(await settings.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
    true,
  );
  await closeSettingsWindow(settings);
  await expect(page.getByRole("searchbox", { name: "搜索市场合约" })).toHaveValue("rb19");
  const reopened = await openSettingsWindow(page);
  await expect(reopened.getByRole("heading", { name: "交易服务", exact: true })).toBeVisible();
  await reopened.setViewportSize({ width: 640, height: 440 });
  expect(await reopened.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
    true,
  );
  await closeSettingsWindow(reopened);
});
