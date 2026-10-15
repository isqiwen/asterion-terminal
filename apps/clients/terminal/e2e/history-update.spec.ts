import { test, expect } from "./test";
import { seedHistory, rpc } from "./dataset-fixture";

test("fixed archive previews incremental ranges and whole-day repair without submitting", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101], "update-preview", {
    product: "zn",
    minuteDays: ["2026-09-21", "2026-09-23"],
    dailyDays: ["2026-09-21", "2026-09-22", "2026-09-23"],
  });
  const before = await rpc(page.request, "data.datasets", {
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
  await expect(extension).toContainText("本次下载：2026-09-23 09:00:01 — 2026-09-26 23:59:59");
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
  await expect(repair).toContainText("本次下载：2026-09-21 09:00:00 — 2026-09-23 09:00:00");
  await expect(repair).toContainText("待核对 1 个交易日：2026-09-22");
  await expect(repair.getByRole("button", { name: "开始下载此范围", exact: true })).toBeDisabled();
  await page.screenshot({ path: "build/history-update-repair.png", fullPage: true });
  const after = await rpc(page.request, "data.datasets", {
    venue: "",
    product: "zn",
    contract_id: "",
    source: "",
  });
  expect(after.history_datasets).toEqual(before.history_datasets);
  expect(
    (await rpc(page.request, "runtime.snapshot")).task_service.tasks.some((task: { id: string }) =>
      task.id.startsWith("history-"),
    ),
  ).toBe(false);
});

test("reviewed closure conflicts explain blocked repair and reject submit before authorization", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101, 102, 103], "closure-gate", {
    minuteDays: ["2026-09-23", "2026-09-25"],
    dailyDays: ["2026-09-23", "2026-09-24", "2026-09-25"],
  });
  const filter = { venue: "", product: "rb", contract_id: "", source: "" };
  const before = await rpc(page.request, "data.datasets", filter);
  const evidence = await rpc(page.request, "data.minutes.evidence", {
    dataset_id: seeded.source_dataset_ids[0],
    offset: 0,
    limit: 50,
  });
  const beforeTables = await rpc(page.request, "data.tables.coverage");
  const tasks = (await rpc(page.request, "runtime.snapshot")).task_service.tasks.map(
    (task: { id: string }) => task.id,
  );
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  await archive.getByLabel("品种", { exact: true }).fill("rb");
  await archive.getByRole("button", { name: "覆盖核对", exact: true }).click();
  await archive
    .getByRole("table", { name: "合约覆盖" })
    .getByRole("button", { name: "补齐缺口", exact: true })
    .click();
  const repair = archive.getByRole("region", { name: "补齐缺口", exact: true });
  await repair.getByRole("button", { name: "预览下载范围", exact: true }).click();
  const alert = repair.getByRole("alert");
  await expect(alert).toContainText("公告冲突阻止整日补齐预览");
  await expect(alert).toContainText("2026-09-25");
  await expect(alert).toContainText("所选日历标记开市");
  await expect(alert.getByRole("link")).toHaveAttribute(
    "href",
    "https://www.shfe.com.cn/publicnotice/notice/202609/t20260921_833503.html",
  );
  await expect(alert).toContainText("重新打开历史数据仓库");
  await expect(repair.getByRole("button", { name: "开始下载此范围", exact: true })).toHaveCount(0);
  await expect(repair.getByRole("button", { name: "应用共享额度", exact: true })).toHaveCount(0);
  const query = {
    dataset_id: seeded.source_dataset_ids[0],
    daily: seeded.daily,
    mode: "repair",
    end_day: "",
    requests_per_minute: 60,
  };
  const blocked = (await rpc(page.request, "data.download.update.plan", query)).history_update_plan;
  expect(blocked.conflicts[0].day).toBe("2026-09-25");
  // This deliberately rejected isolated call proves the gate runs before credentials or authorization.
  const rejected = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "data.download.update.submit",
      params: { id: "blocked-fixture", query, plan_id: blocked.id, token: "" },
    },
  });
  const response = await rejected.json();
  expect(response.error.code).toBe("conflict");
  expect(response.error.message).toContain(
    "repair preview is blocked by reviewed closure conflicts",
  );
  expect((await rpc(page.request, "data.datasets", filter)).history_datasets).toEqual(
    before.history_datasets,
  );
  expect(
    (
      await rpc(page.request, "data.minutes.evidence", {
        dataset_id: seeded.source_dataset_ids[0],
        offset: 0,
        limit: 50,
      })
    ).minute_evidence,
  ).toEqual(evidence.minute_evidence);
  expect((await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot).toBe(
    beforeTables.table_coverage.snapshot,
  );
  expect(
    (await rpc(page.request, "runtime.snapshot")).task_service.tasks.map(
      (task: { id: string }) => task.id,
    ),
  ).toEqual(tasks);
});
