import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";

test("F10 explicit fixed-data slippage variants preserve baseline and show sensitivity separately", async ({
  page,
}) => {
  test.setTimeout(90000);
  const prices = [100, 102, 106, 110, 104, 98, 94, 100, 107, 112, 103, 95];
  const days = Array.from({ length: 20 }, (_, i) =>
    new Date(Date.UTC(2026, 8, 7 + i)).toISOString().slice(0, 10),
  )
    .filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()))
    .slice(0, prices.length);
  const seeded = await seedHistory(page.request, prices, "f10-daily", {
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
  await rpc(page.request, "backtest.submit", {
    id: "f10-baseline",
    strategies: [{ quantity: "1", sides: "both", rule: { kind: "momentum", lookback: 2 } }],
    holdout_from: "",
    walk_forward: null,
    deposit: "100000",
    max_order_quantity: "1",
    max_gross_quantity: "1",
    max_working_orders: "4",
    contracts: [
      {
        venue: "SHFE",
        symbol: "rb2610",
        slippage_ticks: 0,
        cost_schedule: [
          {
            effective_from: "1970-01-01",
            source: "isolated explicit test cost",
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
  const tasks = async () => (await rpc(page.request, "runtime.snapshot")).task_service.tasks;
  await expect
    .poll(async () => (await tasks()).find((t: { id: string }) => t.id === "f10-baseline")?.state)
    .toBe("succeeded");
  const original = (await rpc(page.request, "task.result.read", { id: "f10-baseline" }))
    .task_result;
  expect(original.result.account.fills.length).toBeGreaterThan(0);
  const before = (await tasks()).map((t: { id: string }) => t.id);
  await page.goto("/");
  await openView(page, "backtest");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "f10-baseline" })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  const panel = workspace.getByRole("region", { name: "固定数据滑点敏感性", exact: true });
  const input = panel.getByLabel("滑点变体（1–2个整数，以逗号分隔）", { exact: true });
  await input.fill("1,2");
  await panel.getByRole("button", { name: "核对滑点变体", exact: true }).click();
  await expect(panel).toContainText("0 → 1, 2");
  await panel.getByRole("button", { name: "取消滑点变体", exact: true }).click();
  expect((await tasks()).map((t: { id: string }) => t.id)).toEqual(before);
  await input.fill("1,2");
  await panel.getByRole("button", { name: "核对滑点变体", exact: true }).click();
  await panel.getByRole("button", { name: "确认创建滑点变体", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await tasks()).filter(
          (t: { id: string; state: string }) => t.id !== "f10-baseline" && t.state === "succeeded",
        ).length,
    )
    .toBe(2);
  const variants = (await tasks()).filter((t: { id: string }) => t.id !== "f10-baseline");
  for (const task of variants) {
    const result = (await rpc(page.request, "task.result.read", { id: task.id })).task_result;
    const ticks = result.experiment.paper.contracts[0].slippage_ticks;
    expect([1, 2]).toContain(ticks);
    const normalized = structuredClone(result.experiment);
    normalized.paper.contracts[0].slippage_ticks = 0;
    expect(normalized).toEqual(original.experiment);
    expect(result.task.metadata.source_id).toBe("f10-baseline");
    expect(result.task.metadata.data_choice).toBe("fixed");
    expect(result.task.risk_artifact).toBe(original.task.risk_artifact);
    expect(result.result.account.equity).not.toBe(original.result.account.equity);
  }
  expect((await rpc(page.request, "task.result.read", { id: "f10-baseline" })).task_result).toEqual(
    original,
  );
  await panel.getByRole("button", { name: "读取滑点敏感性结果", exact: true }).click();
  const table = panel.getByRole("table", { name: "滑点敏感性结果", exact: true });
  await expect(table.locator("tbody tr")).toHaveCount(3);
  await expect(panel).toContainText("不排序、不宣称最优");
  const comparison = workspace.getByRole("region", { name: "回测实验比较", exact: true });
  for (const id of ["f10-baseline", variants[0].id])
    await comparison.getByRole("checkbox", { name: new RegExp(id) }).check();
  await comparison.getByRole("button", { name: "读取并比较", exact: true }).click();
  await expect(comparison.getByRole("table", { name: "回测实验对照", exact: true })).toContainText(
    "滑点不同",
  );
});
