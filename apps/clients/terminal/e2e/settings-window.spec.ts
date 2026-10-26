import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";

test("opening connection settings only reads the monitor established by startup", async ({
  page,
}) => {
  await page.goto("/");
  const settings = await openSettingsWindow(page);
  const commands: string[] = [];
  await settings.route("**/__asterion/api", route => {
    const { method } = route.request().postDataJSON();
    if (method !== "runtime.snapshot") {
      commands.push(method);
      return route.fulfill({
        json: {
          error: {
            code: "conflict",
            message: "another Terminal operation is in progress; retry after it completes",
          },
        },
      });
    }
    return route.continue();
  });
  for (let visit = 0; visit < 2; visit++) {
    await settings.getByRole("button", { name: "连接与部署", exact: true }).click();
    await expect(settings.getByRole("list", { name: "当前运行位置" })).toContainText("research");
    await settings.getByRole("button", { name: "机器管理", exact: true }).click();
    await expect(settings.getByRole("button", { name: "本机 在线", exact: true })).toBeVisible();
    await expect(settings.getByRole("alert")).toHaveCount(0);
    await settings.getByRole("button", { name: "偏好设置", exact: true }).click();
  }
  expect(commands).toEqual([]);
  await closeSettingsWindow(settings);
});

test("settings reuses its window and preserves workbench drafts", async ({ page }) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "history");
  await page.getByLabel("每分钟请求上限").fill("17");
  const settings = await openSettingsWindow(page);
  await settings.setViewportSize({ width: 760, height: 540 });
  await expect(settings.getByRole("heading", { name: "偏好设置", exact: true })).toBeVisible();
  await expect(page.getByLabel("每分钟请求上限")).toHaveValue("17");
  await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
  await expect(page.locator("html")).toHaveAttribute("lang", "en-US");
  await settings.getByLabel("Display Density").selectOption("comfortable");
  await settings.screenshot({ path: "apps/clients/terminal/test-results/settings-window.png" });
  await settings.getByRole("button", { name: "About", exact: true }).click();
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "About", exact: true })).toBeVisible();
  await settings.keyboard.press("Control+,");
  await expect(settings.getByRole("heading", { name: "About", exact: true })).toBeVisible();
  await page.getByLabel("Requests per minute", { exact: true }).fill("19");
  await settings.getByRole("button", { name: "Preferences", exact: true }).click();
  expect(page.context().pages()).toHaveLength(2);
  await settings.getByLabel("Language").selectOption("zh-CN");
  await page.getByRole("button", { name: "查看服务连接", exact: true }).click();
  await page.getByRole("button", { name: "连接设置", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "连接与部署", exact: true })).toBeVisible();
  expect(page.context().pages()).toHaveLength(2);
  await settings.screenshot({
    path: "apps/clients/terminal/test-results/settings-connections.png",
  });
  expect(await settings.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
    true,
  );
  await closeSettingsWindow(settings);
  await expect(page.getByLabel("每分钟请求上限")).toHaveValue("19");
  const reopened = await openSettingsWindow(page);
  await expect(reopened.getByRole("heading", { name: "连接与部署", exact: true })).toBeVisible();
  await reopened.setViewportSize({ width: 640, height: 440 });
  expect(await reopened.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
    true,
  );
  await closeSettingsWindow(reopened);
});
