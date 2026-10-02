import { test, expect } from "@playwright/test";
import { seedHistory, rpc } from "./dataset-fixture";

test("fixed archive previews incremental ranges and whole-day repair without submitting", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101], "update-preview", {
    product: "zn",
    minuteDays: ["2026-09-23", "2026-09-25"],
    dailyDays: ["2026-09-23", "2026-09-24", "2026-09-25"],
  });
  const before = await rpc(page.request, "research.datasets", {
    venue: "",
    product: "zn",
    contract_id: "",
    source: "",
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  const row = archive.locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`);
  await row.getByRole("button", { name: "下载后续数据", exact: true }).click();
  const extension = archive.getByRole("region", { name: "下载后续数据", exact: true });
  await extension.getByLabel("下载至日期").fill("2026-09-26");
  await extension.getByRole("button", { name: "预览下载范围", exact: true }).click();
  await expect(extension).toContainText("本次下载：2026-09-25 09:00:01 — 2026-09-26 23:59:59");
  await extension.getByLabel("下载至日期").fill("2026-09-27");
  await expect(extension.getByRole("button", { name: "开始下载此范围", exact: true })).toHaveCount(
    0,
  );
  await extension.getByRole("button", { name: "预览下载范围", exact: true }).click();
  await expect(extension).toContainText("2026-09-27 23:59:59");
  await extension.getByRole("button", { name: "关闭", exact: true }).click();
  // Change the filter without querying the list first: coverage actions must
  // still resolve their own fixed archive rows.
  await archive.getByLabel("品种", { exact: true }).fill("rb");
  await archive.getByRole("button", { name: "查询", exact: true }).click();
  await expect(row).toHaveCount(0);
  await archive.getByLabel("品种", { exact: true }).fill("zn");
  await archive.getByRole("button", { name: "覆盖核对", exact: true }).click();
  const coverage = archive.getByRole("table", { name: "合约覆盖" });
  await coverage.getByRole("button", { name: "补齐缺口", exact: true }).click();
  const repair = archive.getByRole("region", { name: "补齐缺口", exact: true });
  await expect(repair).toContainText("重新下载原始请求时段");
  await repair.getByRole("button", { name: "预览下载范围", exact: true }).click();
  await expect(repair).toContainText("本次下载：2026-09-23 09:00:00 — 2026-09-25 09:00:00");
  await expect(repair).toContainText("待核对 1 个交易日：2026-09-24");
  await expect(repair.getByRole("button", { name: "开始下载此范围", exact: true })).toBeDisabled();
  await page.screenshot({ path: "build/history-update-repair.png", fullPage: true });
  const after = await rpc(page.request, "research.datasets", {
    venue: "",
    product: "zn",
    contract_id: "",
    source: "",
  });
  expect(after.history_datasets).toEqual(before.history_datasets);
  expect(
    (await rpc(page.request, "runtime.snapshot")).research.tasks.some((task: { id: string }) =>
      task.id.startsWith("history-"),
    ),
  ).toBe(false);
});
