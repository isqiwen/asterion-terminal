import { seedHistory } from "./dataset-fixture";
import { test, expect } from "@playwright/test";

test("archive coverage check lists trading days per contract", async ({ page }) => {
  await page.goto("/");
  await seedHistory(page.request, [100, 101, 102], "coverage");
  await page.reload();
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  await expect(archive.getByRole("button", { name: "覆盖核对", exact: true })).toBeDisabled();
  await archive.getByLabel("品种", { exact: true }).fill("rb");
  await archive.getByRole("button", { name: "覆盖核对", exact: true }).click();
  const table = archive.getByRole("table", { name: "合约覆盖" });
  const row = table.getByRole("row").filter({ hasText: "SHFE/rb/2026-10" });
  await expect(row).toContainText("1 · 2026-09-25 — 2026-09-25");
  await expect(row.getByRole("cell").last()).toHaveText("无");
});
