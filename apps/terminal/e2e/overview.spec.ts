import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";

test("overview keeps empty summaries compact and preserves saved layouts", async ({ page }) => {
  const key = "asterion.cpp-terminal.dashboard.v1";
  const layout = JSON.stringify({
    version: 1,
    refreshSeconds: 0,
    compact: false,
    pulse: false,
    items: [
      { id: "market.quotes", width: 2 },
      { id: "data.local", width: 2 },
      { id: "trading.portfolio", width: 1 },
      { id: "trading.risk-metrics", width: 1 },
    ],
  });
  await page.addInitScript(({ key, layout }) => localStorage.setItem(key, layout), { key, layout });
  // Explicit empty-state fixture; no fabricated market prices or account balances.
  // Polls keep arriving while the test finishes; unrouteAll below ignores
  // handlers whose responses were disposed with the page.
  await page.route("**/__asterion/api", async route => {
    const response = await route.fetch();
    if (route.request().postDataJSON()?.method !== "runtime.snapshot") {
      await route.fulfill({ response });
      return;
    }
    const value = await response.json();
    Object.assign(value.result, { market: null, paper: null, dataset: null });
    await route.fulfill({ response, json: value });
  });
  await page.goto("/");
  await expect(page.getByRole("heading", { name: "自选行情", exact: true })).toBeVisible();
  await expect(page.getByRole("heading", { name: "风险概览", exact: true })).toHaveCount(0);
  await expect(page.getByRole("heading", { name: "数据工作台", exact: true })).toHaveCount(0);
  await expect(page.getByText("工作概览", { exact: true })).toHaveCount(0);
  await expect(page.getByText("本地 CSV 已就绪", { exact: true })).toHaveCount(0);
  expect(await page.evaluate(key => localStorage.getItem(key), key)).toBe(layout);
  await page.screenshot({ path: "apps/terminal/test-results/overview-empty.png" });
  await page.setViewportSize({ width: 800, height: 900 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.getByRole("button", { name: "编辑布局", exact: true }).click();
  await expect(page.getByRole("heading", { name: "数据工作台", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "取消编辑", exact: true }).click();
  page = await openSettingsWindow(
    page,
    page.getByRole("button", { name: "查看服务状态", exact: true }),
  );
  await expect(page.getByRole("heading", { name: "连接与部署", exact: true })).toBeVisible();
  page = await closeSettingsWindow(page);
  await page.getByRole("button", { name: "连接行情", exact: true }).click();
  await expect(page.getByRole("tab", { name: "实时行情", exact: true })).toHaveAttribute(
    "aria-selected",
    "true",
  );
  await page.unrouteAll({ behavior: "ignoreErrors" });
});
