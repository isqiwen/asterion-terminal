import { test, expect } from "@playwright/test";

test("read-only market workspace receives C++ test SDK quotes without storing credentials", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const panel = page.getByRole("region", { name: "实时期货行情" });
  await panel.getByLabel("实际合约", { exact: true }).fill("rb2610");
  await panel.getByRole("button", { name: "添加自选" }).click();
  await panel.getByLabel("行情前置", { exact: true }).fill("tcp://127.0.0.1:1");
  await panel.getByLabel("经纪商代码", { exact: true }).fill("test");
  await panel.getByLabel("用户代码", { exact: true }).fill("fixture");
  await panel.getByLabel("密码", { exact: true }).fill("ui-fixture-secret");
  const connect = panel.getByRole("button", { name: "连接行情", exact: true });
  await expect(panel.getByRole("status")).toHaveText("未登录");
  await expect(panel.getByRole("button", { name: "启动本机行情服务" })).toHaveCount(0);
  await expect(connect).toBeEnabled();
  await connect.click();
  await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  await expect(panel.getByRole("cell", { name: "3509 / 2", exact: true })).toBeVisible();
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "ui-fixture-secret",
  );
  await expect(panel.getByLabel("密码", { exact: true })).toHaveValue("");
  await page.getByRole("button", { name: "总览", exact: true }).click();
  const overview = page.getByRole("region", { name: "自选行情", exact: true });
  await expect(overview.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  await page.screenshot({ path: "apps/terminal/test-results/overview-live.png" });
  await overview.getByRole("button", { name: "管理自选", exact: true }).click();
  await page.screenshot({ path: "apps/terminal/test-results/live-market.png" });
  await panel.getByRole("button", { name: "断开行情", exact: true }).click();
  await expect(panel.getByRole("status")).toHaveText("未登录");
  await expect(panel.getByRole("cell", { name: "断线 · 旧报价", exact: true })).toBeVisible();
});
