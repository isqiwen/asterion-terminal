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

test("settings service failures stay in the main window notification center", async ({ page }) => {
  await page.goto("/");
  const settings = await openSettingsWindow(page);
  await settings.setViewportSize({ width: 820, height: 540 });
  await settings.route("**/__asterion/api", route => {
    if (route.request().postDataJSON().method !== "warehouse.call") return route.continue();
    return route.fulfill({
      json: {
        error: { code: "conflict", message: "warehouse protocol differs from this Terminal" },
      },
    });
  });
  await settings.getByRole("button", { name: "数据服务", exact: true }).click();
  await expect(settings.getByText("数据服务与 Terminal 的通信协议不一致")).toBeVisible();
  await expect(settings.locator(".status-bar, .notification-center")).toHaveCount(0);
  const latest = page.locator(".notification-latest");
  await expect(latest).toContainText("数据服务与 Terminal 的通信协议不一致");
  await latest.click();
  const notices = page.getByRole("region", { name: "通知", exact: true });
  await expect(notices.locator("li")).toHaveCount(1);
  await expect(notices.locator(".notification-source")).toHaveText("数据");
  await expect(notices.locator(".error-notice")).toHaveAttribute(
    "title",
    /conflict: warehouse protocol differs from this Terminal/,
  );
  await settings.screenshot({ path: "build/settings-without-notifications.png" });
  await page.screenshot({ path: "build/settings-error-main-notification.png" });

  await settings.getByRole("button", { name: "编辑数据连接", exact: true }).click();
  await settings.getByLabel("数据服务证书指纹", { exact: true }).fill("bad-fingerprint");
  await settings.getByRole("button", { name: "保存连接", exact: true }).click();
  await expect(settings.getByRole("alert")).toBeVisible();
  await expect(notices.locator("li")).toHaveCount(1);

  await settings.getByRole("button", { name: "偏好设置", exact: true }).click();
  await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
  await expect(page.locator(".notification-kept")).toContainText("Notifications");
  await expect(page.getByRole("region", { name: "Notifications", exact: true })).toContainText(
    "Warehouse and Terminal use different communication protocols",
  );
  await settings.getByLabel("Language", { exact: true }).selectOption("zh-CN");
  await closeSettingsWindow(settings);
  await expect(notices.locator("li")).toHaveCount(1);
  await notices.getByRole("button", { name: "清空", exact: true }).click();
  const reopened = await openSettingsWindow(page);
  await expect(reopened.getByRole("heading", { name: "偏好设置", exact: true })).toBeVisible();
  await expect(notices).toBeHidden();
  await page.getByRole("button", { name: "通知", exact: true }).click();
  await expect(page.getByRole("region", { name: "通知", exact: true })).toContainText("没有消息");
  await expect(reopened.locator(".status-bar, .notification-center")).toHaveCount(0);
  await closeSettingsWindow(reopened);
});
