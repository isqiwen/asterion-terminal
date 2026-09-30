import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { join } from "node:path";

test("workbench with source-only navigation and existing C++ data", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible();
  await expect(page.getByRole("navigation", { name: "业务工作区" })).toBeVisible();
  await expect(page.locator(".rail-toggle")).toHaveCount(0);
  await page.keyboard.press("Control+b");
  await expect(page.locator(".activity-rail")).toHaveClass(/collapsed/);
  await expect(page.locator(".workspace-tabs .nav-label").first()).toBeHidden();
  await page.keyboard.press("Meta+b");
  await expect(page.locator(".activity-rail")).not.toHaveClass(/collapsed/);
  await expect(page.locator(".workspace-tabs .nav-label").first()).toBeVisible();
  await page.getByRole("button", { name: "编辑布局", exact: true }).click();
  await page.getByRole("button", { name: "取消编辑", exact: true }).click();
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  await expect(page.getByLabel("CSV 文件路径")).toHaveCount(0);
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "插件", exact: true }).click();
  await expect(
    page.getByRole("cell", { name: "asterion.risk.order-limits", exact: true }),
  ).toBeVisible();
  page = await closeSettingsWindow(page);
  await page.getByRole("button", { name: "总览", exact: true }).click();
  await expect(page.getByRole("button", { name: "总览", exact: true })).toHaveAttribute(
    "aria-current",
    "page",
  );
  await page.screenshot({ path: join(__dirname, "../test-results/workbench.png") });
});

test("core connection failure is visible and can be retried", async ({ page }) => {
  await page.route("**/__asterion/api", route =>
    route.fulfill({ status: 503, body: "unavailable" }),
  );
  await page.goto("/");
  await expect(page.getByRole("alert")).toContainText("本机 C++ 服务不可用");
  await page.unroute("**/__asterion/api");
  await page.getByRole("button", { name: "重试启动", exact: true }).click();
  await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible();
});
