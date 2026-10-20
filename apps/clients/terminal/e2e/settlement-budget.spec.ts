import { openView } from "./navigation-helper";
import { readFile, writeFile } from "node:fs/promises";
import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";

test("recorded settlement budgets reveal earlier excess while the final account is within", async ({
  page,
}, info) => {
  test.setTimeout(90000);
  const days = ["2026-09-07", "2026-09-08", "2026-09-09", "2026-09-10", "2026-09-11", "2026-09-14"];
  await seedHistory(page.request, [100, 110, 110, 99, 99, 99], "settled-rb", {
    product: "rb",
    tablesOnly: true,
    dailyPrices: true,
    dailyDays: days,
  });
  const seeded = await seedHistory(page.request, [100, 90, 90, 99, 99, 99], "settled-zn", {
    product: "zn",
    keep: true,
    tablesOnly: true,
    dailyPrices: true,
    dailyDays: days,
  });
  await rpc(page.request, "data.dataset.clear");
  for (const product of ["rb", "zn"])
    await rpc(page.request, "data.dataset.daily", {
      contract_id: `SHFE/${product}/2026-10`,
      daily: seeded.daily,
      begin_day: days[0],
      end_day: days.at(-1),
    });
  const costs = {
    effective_from: "1970-01-01",
    source: "settlement-path explicit synthetic assumption",
    values: {
      margin_per_lot: "100",
      margin_rate: "0",
      open_fee: "1",
      close_today_fee: "1",
      close_yesterday_fee: "1",
      open_fee_rate: "0",
      close_today_fee_rate: "0",
      close_yesterday_fee_rate: "0",
    },
  };
  await rpc(page.request, "backtest.submit", {
    id: "settlement-path",
    strategies: [
      {
        sides: "both",
        rule: {
          kind: "cross_momentum",
          lookback: 1,
          rebalance: 2,
          count: 1,
          notional: "1000",
          volatility: 0,
          reverse: false,
        },
      },
    ],
    holdout_from: "",
    walk_forward: null,
    deposit: "100000",
    max_order_quantity: "1",
    max_gross_quantity: "2",
    max_working_orders: "4",
    contracts: ["rb", "zn"].map(product => ({
      venue: "SHFE",
      symbol: `${product}2610`,
      slippage_ticks: 0,
      cost_schedule: [costs],
    })),
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).task_service.tasks.find(
          (t: { id: string }) => t.id === "settlement-path",
        )?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const original = (await rpc(page.request, "task.result.read", { id: "settlement-path" }))
    .task_result;
  const points = original.portfolio.settlement_path.points;
  expect(points).toHaveLength(days.length);
  expect(
    points.every(
      (p: { gross_notional: string | null; model_margin: string | null }) =>
        p.gross_notional !== null && p.model_margin !== null,
    ),
  ).toBe(true);
  expect(original.portfolio.gross_notional).toBe("1980");
  expect(original.portfolio.net_notional).toBe("0");
  expect(points.some((p: { gross_notional: string }) => Number(p.gross_notional) > 1980)).toBe(
    true,
  );
  expect(original.result.settlements.every((d: { occupancy?: unknown }) => !!d.occupancy)).toBe(
    true,
  );
  const beforeTasks = (await rpc(page.request, "runtime.snapshot")).task_service.tasks
    .map((t: { id: string }) => t.id)
    .sort();
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  await openView(page, "backtest");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "查看结果", exact: true }).click();
  await workspace.getByText("组合敞口与预算复核", { exact: true }).click();
  await expect(workspace.getByLabel("总名义限额（期末与结算点复核）", { exact: true })).toHaveValue(
    "",
  );
  await workspace.getByLabel("总名义限额（期末与结算点复核）", { exact: true }).fill("1980");
  await workspace.getByRole("button", { name: "复核记录点预算", exact: true }).click();
  const final = workspace.getByRole("table", { name: "期末预算复核结果", exact: true });
  await expect(final.getByRole("row").nth(1)).toContainText("复核范围内（非执行许可）");
  await workspace.getByText("逐结算日占用与预算轨迹", { exact: true }).click();
  const table = workspace.getByRole("table", { name: "逐结算日预算轨迹", exact: true });
  await expect(table.getByRole("row")).toHaveCount(days.length + 1);
  await expect(table.getByRole("row").filter({ hasText: days[2] })).toContainText(
    "复核超限（非执行拦截）",
  );
  await expect(workspace.getByText(/总名义结算点复核/)).toContainText("未知点 0");
  await expect(workspace.getByText(/跨品种代数净额为零不代表风险对冲或风险为零/)).toBeVisible();
  const review = (
    await rpc(page.request, "backtest.budget.review", {
      id: original.id,
      input_sha256: original.input_sha256,
      result_digest: original.task.result_digest,
      limits: { gross_notional: "1980", margin: null },
    })
  ).portfolio_budget_review;
  expect(review.checks.gross_notional.headroom).toBe("0");
  expect(review.settlement_summary.gross_notional.exceeded_points).toBeGreaterThan(0);
  expect(review.settlement_summary.gross_notional.unknown_points).toBe(0);
  expect(review.settlement_summary.margin.exceeded_points).toBeNull();
  await table.scrollIntoViewIfNeeded();
  const shot = info.outputPath("settlement-budget-path.png");
  await page.screenshot({ path: shot, fullPage: true });
  await info.attach("recorded settlement path", { path: shot, contentType: "image/png" });
  const evidence = info.outputPath("settlement-budget-review.json");
  await writeFile(evidence, JSON.stringify({ original, review }, null, 2));
  await info.attach("fixed observed settlement evidence", {
    path: evidence,
    contentType: "application/json",
  });
  const downloadEvent = page.waitForEvent("download");
  await workspace.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const download = await downloadEvent;
  const path = info.outputPath("settlement-research-evidence.json");
  await download.saveAs(path);
  const exported = JSON.parse(await readFile(path, "utf8"));
  expect(exported.result).toEqual(original.result);
  expect(exported.portfolio).toEqual(original.portfolio);
  expect((await rpc(page.request, "task.result.read", { id: original.id })).task_result).toEqual(
    original,
  );
  expect(
    (await rpc(page.request, "runtime.snapshot")).task_service.tasks
      .map((t: { id: string }) => t.id)
      .sort(),
  ).toEqual(beforeTasks);
});
