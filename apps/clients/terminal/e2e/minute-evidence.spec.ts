import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { seedHistory } from "./dataset-fixture";

test("minute observations read the immutable dataset through the data service", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101, 102, 103], "minute-evidence", {
    minuteDays: ["2026-09-23", "2026-09-25"],
    dailyDays: ["2026-09-23", "2026-09-24", "2026-09-25"],
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  await archive
    .locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`)
    .getByRole("button", { name: "查看数据", exact: true })
    .click();
  const viewer = archive.getByRole("region", { name: "历史数据查看", exact: true });
  await expect(
    viewer.getByText("下载处理已完成；首尾交易日覆盖与日内完整性尚未验证。", { exact: true }),
  ).toBeVisible();
  await viewer.getByText("质量与来源", { exact: true }).click();
  await viewer.getByRole("button", { name: "按交易日检查", exact: true }).click();
  const evidence = viewer.getByRole("region", { name: "交易日记录证据", exact: true });
  await expect(evidence).toContainText("共 4 条记录，2 个已标记交易日；0 条记录缺少交易日标记。");
  const days = evidence.getByRole("table").locator("tbody tr");
  await expect(days).toHaveCount(2);
  await expect(days.nth(0)).toContainText("2026-09-23");
  await expect(days.nth(1)).toContainText("2026-09-25");
  await expect(evidence.getByRole("cell", { name: "未验证", exact: true })).toHaveCount(2);
  await expect(evidence).toContainText("原始请求时段");
});
