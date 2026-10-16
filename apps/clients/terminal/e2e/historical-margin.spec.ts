import { readFile } from "node:fs/promises";
import { rpc, seedHistory } from "./dataset-fixture";
import { test, expect } from "./test";
import { checkSnapshot } from "./snapshot-schema";

test("SHFE historical margin evidence stays fixed through worker execution, reuse and result export", async ({
  page,
}, info) => {
  test.setTimeout(90000);
  const days = ["2025-10-20", "2025-10-21", "2025-10-22"];
  const { daily } = await seedHistory(page.request, [100, 101, 102], "historical-margin", {
    dailyDays: days,
    tablesOnly: true,
    dailyPrices: true,
    shfeMargins: true,
    product: "au",
    month: "2025-12",
  });
  await rpc(page.request, "data.dataset.daily", {
    contract_id: "SHFE/au/2025-12",
    daily,
    begin_day: days[0],
    end_day: days[2],
  });
  const preview = (await rpc(page.request, "backtest.margins.preview")).historical_margins;
  expect(
    preview.days.map((day: { day_start_rate: string; settlement_rate: string }) => [
      day.day_start_rate,
      day.settlement_rate,
    ]),
  ).toEqual([
    ["0.14", "0.14"],
    ["0.14", "0.16"],
    ["0.16", "0.16"],
  ]);
  const costs = {
    margin_per_lot: "0",
    margin_rate: "0.14",
    open_fee: "2",
    close_today_fee: "3",
    close_yesterday_fee: "4",
    open_fee_rate: "0",
    close_today_fee_rate: "0",
    close_yesterday_fee_rate: "0",
  };
  await rpc(page.request, "backtest.submit", {
    id: "historical-margin",
    deposit: "10000",
    max_order_quantity: "10",
    max_gross_quantity: "10",
    max_working_orders: "10",
    strategies: [
      { quantity: "1", sides: "long", rule: { kind: "moving_average", fast: 1, slow: 2 } },
    ],
    holdout_from: "",
    walk_forward: null,
    margin_snapshot: preview.snapshot,
    contracts: [
      {
        venue: "SHFE",
        symbol: "au2512",
        slippage_ticks: 0,
        cost_schedule: [
          { effective_from: days[0], source: "test-only trading costs", values: costs },
        ],
      },
    ],
  });
  async function result(id: string) {
    await expect
      .poll(
        async () =>
          (await rpc(page.request, "runtime.snapshot")).task_service.tasks.find(
            (t: { id: string }) => t.id === id,
          )?.state,
        { timeout: 20000 },
      )
      .toBe("succeeded");
    return (await rpc(page.request, "task.result.read", { id })).task_result;
  }
  const original = await result("historical-margin");
  expect(original.experiment.historical_margins).toEqual(preview);
  expect(original.result.account.fills).toHaveLength(1);
  expect(original.result.settlements[1].margin_revaluation).toMatchObject({
    previous_rate: "0.14",
    rate: "0.16",
  });
  expect(original.result.settlements[2].margin_revaluation).toMatchObject({
    previous_rate: "0.16",
    rate: "0.16",
  });
  expect(original.reconciliation.final_checks.every((c: { matches: boolean }) => c.matches)).toBe(
    true,
  );
  await rpc(page.request, "backtest.reuse", {
    id: "historical-margin-copy",
    source_id: original.id,
    source_result_digest: original.task.result_digest,
    source_input_sha256: original.input_sha256,
    mode: "fixed",
    snapshot: "",
  });
  const copy = await result("historical-margin-copy");
  expect(copy.experiment).toEqual(original.experiment);
  expect(copy.result).toEqual(original.result);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await page
    .getByRole("navigation", { name: "研究页面", exact: true })
    .getByRole("button", { name: "回测", exact: true })
    .click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ has: page.getByText(original.id, { exact: true }) })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  await workspace.getByText("结算保证金影响", { exact: true }).click();
  const impact = workspace.getByRole("table", { name: "结算保证金影响", exact: true });
  await expect(impact.getByRole("row")).toHaveCount(4);
  await expect(impact).toContainText(original.result.settlements[2].occupancy.margin);
  await workspace.getByText("实验参数", { exact: true }).click();
  await expect(workspace.getByRole("table", { name: "历史保证金参数", exact: true })).toContainText(
    "0.16",
  );
  const snapshot = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("settlement margin result", snapshot);
  const downloadEvent = page.waitForEvent("download");
  await workspace.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const download = await downloadEvent;
  const document = JSON.parse(await readFile((await download.path())!, "utf8"));
  expect(document.experiment).toEqual(original.experiment);
  expect(document.result).toEqual(original.result);
  await page.screenshot({ path: info.outputPath("historical-margin.png"), fullPage: true });
});
