import { openView } from "./navigation-helper";
import { readFile } from "node:fs/promises";
import { rpc, seedHistory } from "./dataset-fixture";
import { test, expect } from "./test";
import { checkSnapshot } from "./snapshot-schema";

test("settlement margin stays fixed through worker execution, reuse and result export", async ({
  page,
}, info) => {
  test.setTimeout(90000);
  const days = ["2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24"];
  const { daily } = await seedHistory(page.request, [100, 101, 102, 103], "settlement-margin", {
    dailyDays: days,
    tablesOnly: true,
    dailyPrices: true,
  });
  await rpc(page.request, "data.dataset.daily", {
    contract_id: "SHFE/rb/2026-10",
    daily,
    begin_day: days[0],
    end_day: days[3],
  });
  const events = [
    { trading_day: days[2], rate: "0.16", source: "test-only settlement model" },
    { trading_day: days[3], rate: "0.18", source: "test-only final settlement model" },
  ];
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
    id: "settlement-margin",
    deposit: "10000",
    max_order_quantity: "10",
    max_gross_quantity: "10",
    max_working_orders: "10",
    strategies: [
      { quantity: "1", sides: "long", rule: { kind: "moving_average", fast: 1, slow: 2 } },
    ],
    holdout_from: "",
    walk_forward: null,
    settlement_margins: events,
    contracts: [
      {
        venue: "SHFE",
        symbol: "rb2610",
        slippage_ticks: 0,
        cost_schedule: [
          { effective_from: days[0], source: "test-only trading costs", values: costs },
          {
            effective_from: days[3],
            source: "explicit next-day model",
            values: { ...costs, margin_rate: "0.17" },
          },
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
  const original = await result("settlement-margin");
  expect(original.experiment.settlement_margins).toEqual(events);
  expect(original.result.account.fills).toHaveLength(1);
  expect(original.result.settlements[2].margin_revaluation).toMatchObject({
    previous_rate: "0.14",
    rate: "0.16",
  });
  expect(original.result.settlements[3].margin_revaluation).toMatchObject({
    previous_rate: "0.17",
    rate: "0.18",
  });
  expect(original.reconciliation.final_checks.every((c: { matches: boolean }) => c.matches)).toBe(
    true,
  );
  await rpc(page.request, "backtest.reuse", {
    id: "settlement-margin-copy",
    source_id: original.id,
    source_result_digest: original.task.result_digest,
    source_input_sha256: original.input_sha256,
    mode: "fixed",
    snapshot: "",
  });
  const copy = await result("settlement-margin-copy");
  expect(copy.experiment).toEqual(original.experiment);
  expect(copy.result).toEqual(original.result);
  await page.goto("/");
  await openView(page, "backtest");
  await openView(page, "backtest");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ has: page.getByText(original.id, { exact: true }) })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  await workspace.getByText("结算保证金影响", { exact: true }).click();
  const impact = workspace.getByRole("table", { name: "结算保证金影响", exact: true });
  await expect(impact.getByRole("row")).toHaveCount(3);
  await expect(impact).toContainText(original.result.settlements[3].occupancy.margin);
  await workspace.getByText("实验参数", { exact: true }).click();
  await expect(workspace.getByRole("table", { name: "结算保证金参数", exact: true })).toContainText(
    events[1].source,
  );
  const snapshot = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("settlement margin result", snapshot);
  const downloadEvent = page.waitForEvent("download");
  await workspace.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const download = await downloadEvent;
  const document = JSON.parse(await readFile((await download.path())!, "utf8"));
  expect(document.experiment).toEqual(original.experiment);
  expect(document.result).toEqual(original.result);
  await page.screenshot({ path: info.outputPath("settlement-margin.png"), fullPage: true });
});
