import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("original workbench with real C++ futures preview and failure recovery", async ({ page }) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-terminal-e2e-"));
  const path = join(folder, "trades.csv");
  await writeFile(
    path,
    "timestamp_ns,price,quantity\n1790384400000000000,3510,2\n1790384401000000000,3511,3\n",
  );
  try {
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible();
    await expect(page.getByRole("navigation", { name: "业务工作区" })).toBeVisible();
    await page.getByRole("button", { name: "收起导航", exact: true }).click();
    await expect(page.locator(".activity-rail")).toHaveCSS("width", "40px");
    await page.getByRole("button", { name: "展开导航", exact: true }).click();
    await expect(page.locator(".activity-rail")).toHaveCSS("width", "64px");
    await page.getByRole("button", { name: "编辑布局", exact: true }).click();
    await page.getByRole("button", { name: "取消编辑", exact: true }).click();
    await page.getByRole("button", { name: "数据", exact: true }).click();
    await page.getByLabel("CSV 文件路径").fill(path);
    await page.getByLabel("品种代码", { exact: true }).fill("rb");
    await page.getByLabel("实际合约", { exact: true }).fill("rb2610");
    await page.getByLabel("交割月份", { exact: true }).fill("2026-10");
    await page.getByLabel("价格步长", { exact: true }).fill("1");
    await page.getByLabel("每手乘数", { exact: true }).fill("10");
    await page.getByRole("button", { name: "校验并预览", exact: true }).click();
    await expect(page.getByText("文件校验完成，可在市场工作区查看历史成交。")).toBeVisible();
    await page.getByRole("button", { name: "查看行情", exact: true }).click();
    await expect(page.getByRole("img", { name: "rb2610最近2笔历史成交价格" })).toBeVisible();
    await expect(page.locator(".quote-strip")).toContainText("总成交量 5 手");
    await expect(page.locator(".quote-strip")).toContainText("历史数据 · 非实时");
    await page.screenshot({ path: join(__dirname, "../test-results/futures-market.png") });
    await page.getByRole("button", { name: "数据", exact: true }).click();
    await page.getByLabel("CSV 文件路径").fill(path);
    await page.getByLabel("品种代码", { exact: true }).fill("rb");
    await page.getByLabel("实际合约", { exact: true }).fill("rbMAIN");
    await page.getByLabel("交割月份", { exact: true }).fill("2026-10");
    await page.getByLabel("价格步长", { exact: true }).fill("1");
    await page.getByLabel("每手乘数", { exact: true }).fill("10");
    await page.getByRole("button", { name: "校验并预览", exact: true }).click();
    await page.getByRole("alert").getByRole("button", { name: "详情" }).click();
    await expect(page.getByRole("alert")).toContainText("不接受主力或连续合约别名");
    await page.getByRole("button", { name: "市场", exact: true }).click();
    await expect(page.locator(".quote-strip")).toContainText("3511");
    page = await openSettingsWindow(page);
    await page.getByRole("button", { name: "插件", exact: true }).click();
    await expect(page.getByText("asterion.data.csv", { exact: true })).toBeVisible();
    page = await closeSettingsWindow(page);
    await page.getByRole("button", { name: "总览", exact: true }).click();
    await expect(page.getByRole("tab", { name: "总览", exact: true })).toHaveAttribute(
      "aria-selected",
      "true",
    );
    await page.screenshot({ path: join(__dirname, "../test-results/workbench.png") });
  } finally {
    await rm(folder, { recursive: true, force: true });
  }
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
