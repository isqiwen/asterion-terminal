import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
test("compact footer exposes services, tasks and navigation", async ({ page }) => {
  await page.goto("/");
  const service = page
    .locator(".status-bar")
    .getByRole("button", { name: "查看服务连接", exact: true });
  await expect(service).toBeVisible();
  await expect(page.locator(".watchlist-workspace")).toBeVisible();
  await expect(page.getByRole("button", { name: "检查连接", exact: true })).toHaveCount(0);
  await service.click();
  const panel = page.getByRole("region", { name: "服务连接详情" });
  await expect(panel).toBeVisible();
  await expect(panel).not.toContainText("本机核心");
  await expect(panel).not.toContainText("本机服务");
  await expect(panel).toContainText("此电脑");
  await expect(panel).toContainText("实时行情");
  await expect(panel).toContainText("回测与因子研究");
  await expect(panel.getByText("market-data", { exact: true })).not.toBeVisible();
  await panel.getByText("连接详情", { exact: true }).click();
  await expect(panel.getByText("market-data", { exact: true })).toBeVisible();
  await panel.getByText("连接详情", { exact: true }).click();
  await page.screenshot({ path: "apps/clients/terminal/test-results/status-bar-services.png" });
  await page.keyboard.press("Escape");
  await expect(panel).toHaveCount(0);
  await expect(service).toBeFocused();
  await service.click();
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await expect(panel).toHaveCount(0);
  await page
    .locator(".rail-actions")
    .getByRole("button", { name: "任务中心", exact: true })
    .click();
  await expect(page.getByRole("region", { name: "任务中心", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "关闭任务中心", exact: true }).click();
  await page
    .locator(".status-bar")
    .getByRole("button", { name: /项任务执行中/ })
    .click();
  await expect(page.getByRole("region", { name: "任务中心", exact: true })).toBeVisible();
  await service.click();
  page = await openSettingsWindow(
    page,
    panel.getByRole("button", { name: "连接设置", exact: true }),
  );
  await expect(page.getByRole("list", { name: "当前运行位置", exact: true })).toBeVisible();
});

test("revisioned polls accept unchanged replies from an idle core", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
    timeout: 60000,
  });
  // Exercise the real bridge module in the page, not a copy of its logic.
  const result = await page.evaluate(async () => {
    // Served by the Vite dev server; typed from the same source file.
    const path = "/src/bridge/client.ts";
    const client = (await import(/* @vite-ignore */ path)) as typeof import("../src/bridge/client");
    // A background refresh may publish once between the two calls while
    // services settle; an idle core then answers with the same revision.
    let first = await client.request("runtime.snapshot");
    let poll = await client.pollSnapshot(first.revision);
    for (let attempt = 0; attempt < 5 && !("unchanged" in poll); ++attempt) {
      await new Promise(resolve => setTimeout(resolve, 500));
      first = await client.request("runtime.snapshot");
      poll = await client.pollSnapshot(first.revision);
    }
    return { revision: first.revision, poll };
  });
  expect(result.poll).toMatchObject({ unchanged: true, revision: result.revision });
});
