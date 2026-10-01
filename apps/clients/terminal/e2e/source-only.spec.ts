import { test, expect } from "@playwright/test";
test("all historical data entry points lead to a data source, never a file importer", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await expect(page.getByLabel("数据源", { exact: true })).toHaveValue("tushare.ft_mins");
  await expect(page.getByLabel("CSV 文件路径")).toHaveCount(0);
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  await expect(page.getByLabel("CSV 文件路径")).toHaveCount(0);
  await expect(page.getByText("导入 CSV", { exact: true })).toHaveCount(0);
  await page.getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await page.getByRole("button", { name: "下载历史数据", exact: true }).click();
  await expect(page.getByLabel("数据源", { exact: true })).toBeVisible();
});
