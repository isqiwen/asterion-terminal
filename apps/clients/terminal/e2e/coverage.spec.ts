import { openView } from "./navigation-helper";
import { seedHistory } from "./dataset-fixture";
import { test, expect } from "./test";

// Each minute version is compared with the days the market-wide daily table
// shows its contract traded on.
test("archive coverage check lists trading days per contract", async ({ page }) => {
  const seeded = await seedHistory(page.request, [100, 101, 102], "coverage");
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  await expect(archive.getByRole("button", { name: "覆盖核对", exact: true })).toBeDisabled();
  await archive.getByLabel("品种", { exact: true }).fill("rb");
  await archive.getByRole("button", { name: "覆盖核对", exact: true }).click();
  const table = archive.getByRole("table", { name: "合约覆盖" });
  const row = table.getByRole("row").filter({ hasText: seeded.source_dataset_ids[0].slice(0, 8) });
  await expect(row.getByRole("cell").nth(3)).toHaveText("1 · 2026-09-25 — 2026-09-25");
  await expect(row.getByRole("cell").nth(4)).toHaveText("无");
});
