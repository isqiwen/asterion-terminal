import { openView } from "./navigation-helper";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";
import { join } from "node:path";

test("workbench with source-only navigation and existing C++ data", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".status-bar .status-item").first()).toBeVisible({
    timeout: 60000,
  });
  await expect(page.getByRole("navigation", { name: "业务工作区" })).toBeVisible();
  await expect(page.locator(".rail-toggle")).toHaveCount(0);
  await page.keyboard.press("Control+b");
  await expect(page.locator(".activity-rail")).toHaveClass(/collapsed/);
  await expect(page.locator(".workspace-tabs .nav-label").first()).toBeHidden();
  await page.keyboard.press("Meta+b");
  await expect(page.locator(".activity-rail")).not.toHaveClass(/collapsed/);
  await expect(page.locator(".workspace-tabs .nav-label").first()).toBeVisible();
  await openView(page, "coverage");
  await expect(page.getByLabel("CSV 文件路径")).toHaveCount(0);
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "行情服务", exact: true }).click();
  await expect(page.getByRole("region", { name: "连接", exact: true })).toBeVisible();
  await expect(page.getByRole("region", { name: "Tushare", exact: true })).toHaveCount(0);
  page = await closeSettingsWindow(page);
  await openView(page, "market");
  // The market module has one view, so no view tabs.
  await expect(page.locator(".module-views")).toHaveCount(0);
  await page.screenshot({ path: join(__dirname, "../test-results/workbench.png") });
});

test("core connection failure is visible and can be retried", async ({ page }) => {
  await page.route("**/__asterion/api", route =>
    route.fulfill({ status: 503, body: "unavailable" }),
  );
  await page.goto("/");
  await page.locator(".notification-latest").click();
  await expect(page.getByRole("region", { name: "通知" })).toContainText("本机 C++ 服务不可用");
  await expect(page.getByRole("alert")).toHaveCount(0);
  await page.unroute("**/__asterion/api");
  await page.getByRole("button", { name: "重试", exact: true }).click();
  await expect(page.locator(".status-bar .status-item").first()).toBeVisible({
    timeout: 60000,
  });
});
