import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";

test("verified backtests flow through Polars and skfolio into portfolio research", async ({
  page,
}) => {
  test.setTimeout(90000);
  const days: string[] = [];
  for (let at = Date.UTC(2026, 5, 1); days.length < 60; at += 86400000)
    if (![0, 6].includes(new Date(at).getUTCDay()))
      days.push(new Date(at).toISOString().slice(0, 10));
  const prices = days.map((_, i) => 100 + Math.floor(i / 5) + [0, 4, 8, 3, -2][i % 5]);
  const seeded = await seedHistory(page.request, prices, "portfolio-daily", {
    tablesOnly: true,
    dailyPrices: true,
    dailyDays: days,
  });
  await rpc(page.request, "data.dataset.daily", {
    contract_id: "SHFE/rb/2026-10",
    daily: seeded.daily,
    begin_day: days[0],
    end_day: days.at(-1)!,
  });
  const ids = ["portfolio-low", "portfolio-high"];
  for (const [i, id] of ids.entries())
    await rpc(page.request, "backtest.submit", {
      id,
      strategies: [
        { quantity: String(i + 1), sides: "both", rule: { kind: "momentum", lookback: 2 + i } },
      ],
      holdout_from: "",
      walk_forward: null,
      deposit: "100000",
      max_order_quantity: "4",
      max_gross_quantity: "4",
      max_working_orders: "4",
      contracts: [
        {
          venue: "SHFE",
          symbol: "rb2610",
          slippage_ticks: 0,
          cost_schedule: [
            {
              effective_from: "1970-01-01",
              source: "isolated test cost",
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
            },
          ],
        },
      ],
    });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).task_service.tasks.filter(
          (v: { id: string; state: string }) => ids.includes(v.id) && v.state === "succeeded",
        ).length,
      { timeout: 20000 },
    )
    .toBe(2);
  const sources = [];
  for (const id of ids) {
    const source = (await rpc(page.request, "task.result.read", { id })).task_result;
    expect(source.result.settlements).toHaveLength(60);
    sources.push({ id, result_digest: source.task.result_digest });
  }
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const comparison = page.getByRole("region", { name: "回测实验比较", exact: true });
  for (const id of ids) await comparison.getByRole("checkbox", { name: new RegExp(id) }).check();
  await comparison.getByRole("button", { name: "读取并比较", exact: true }).click();
  const portfolio = comparison.getByRole("region", { name: "回测组合研究", exact: true });
  await portfolio.getByLabel("组合训练截止日（含）", { exact: true }).fill(days[29]);
  await portfolio.getByRole("button", { name: "分析回测组合", exact: true }).click();
  await expect(portfolio.getByRole("img", { name: "组合验证收益率" })).toBeVisible({
    timeout: 30000,
  });
  await expect(portfolio).toContainText("Polars 2.0.0 · skfolio 1.9.0");
  await expect(portfolio).toContainText(`实际验证区间: ${days[30]} – ${days[59]} · 30 个共同观测`);
  const weights = portfolio.getByRole("table", { name: "组合配比与来源" });
  for (const source of sources) await expect(weights).toContainText(source.result_digest);
  await expect(portfolio.getByRole("alert")).toHaveCount(0);
  const stale = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "task.portfolio.analyze",
      params: {
        expected: [{ ...sources[0], result_digest: "0".repeat(64) }, sources[1]],
        training_end: days[29],
      },
    },
  });
  expect((await stale.json()).error.code).toBe("conflict");
  const after = (await rpc(page.request, "runtime.snapshot")).task_service.tasks;
  expect(after).toHaveLength(2);
  for (const source of sources) {
    const verified = (await rpc(page.request, "task.result.read", { id: source.id })).task_result;
    expect(verified.task.result_digest).toBe(source.result_digest);
  }
  await portfolio.screenshot({ path: test.info().outputPath("portfolio-research.png") });
});
