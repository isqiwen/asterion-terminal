import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { seedHistory } from "./dataset-fixture";
test("explicit range audits fixed minute coverage including unobserved target edges", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101], "range", {
    minuteDays: ["2026-09-24"],
    dailyDays: ["2026-09-23", "2026-09-24", "2026-09-25"],
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  const archive = page.getByRole("region", { name: "数据仓库", exact: true });
  await archive
    .locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`)
    .getByRole("button", { name: "查看数据", exact: true })
    .click();
  const audit = archive.getByRole("region", { name: "日期范围覆盖核对", exact: true });
  await audit.getByLabel("目标起始交易日", { exact: true }).fill("2026-09-23");
  await audit.getByLabel("目标结束交易日", { exact: true }).fill("2026-09-26");
  await audit.getByRole("button", { name: "核对目标范围", exact: true }).click();
  const rows = audit.getByRole("table").locator("tbody tr");
  await expect(rows).toHaveCount(4);
  await expect(rows.nth(0)).toContainText("目标交易日未观察到分钟记录");
  await expect(rows.nth(1)).toContainText("已观察到分钟记录");
  await expect(rows.nth(2)).toContainText("本地观测或参考与官方休市公告冲突");
  await expect(rows.nth(3)).toContainText("官方公告明确休市");
  await expect(rows.nth(0).getByRole("cell").nth(3)).toHaveText("—");
  await expect(audit).toContainText(seeded.source_dataset_ids[0]);
  await expect(audit).toContainText(seeded.daily.snapshot);
  await expect(audit).toContainText("未验证");
  await expect(rows.nth(1)).toContainText("该自然日晚间停夜盘");
  await expect(rows.nth(3).getByRole("link", { name: /SHFE/ }).first()).toHaveAttribute(
    "href",
    "https://www.shfe.com.cn/publicnotice/notice/202609/t20260921_833503.html",
  );
});
