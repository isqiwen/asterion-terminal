import { openView } from "./navigation-helper";
import { seedHistory, rpc } from "./dataset-fixture";
import { test, expect } from "./test";
test("daily factor persists liquidity eligibility separately from evaluated members", async ({
  page,
}) => {
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  const days = Array.from({ length: 50 }, (_, i) =>
    new Date(Date.UTC(2026, 7, 20 + i)).toISOString().slice(0, 10),
  ).filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()));
  for (const [i, product] of ["rb", "cu", "al"].entries())
    await seedHistory(page.request, [100], `liquidity-${product}`, {
      product,
      tablesOnly: true,
      dailyDays: days,
      keep: i > 0,
      openInterest: product === "cu" ? 50 : 200,
    });
  await page.reload();
  await openView(page, "backtest");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await openView(page, "factor");
  await workspace.getByRole("button", { name: "全市场数据表", exact: true }).click();
  const input = workspace.getByLabel("品种或合约", { exact: true });
  for (const product of ["rb", "cu", "al"]) {
    await input.fill(`SHFE/${product}/2026-10`);
    await workspace.getByRole("button", { name: "添加", exact: true }).click();
  }
  await workspace.getByLabel("起始日期", { exact: true }).fill(days[0]);
  await workspace.getByLabel("结束日期", { exact: true }).fill(days.at(-1)!);
  await workspace.getByLabel("回看日线数", { exact: true }).fill("2");
  await workspace.getByLabel("未来日线数", { exact: true }).fill("1");
  await workspace.getByLabel("启用历史流动性筛选", { exact: true }).check();
  await workspace.getByLabel("此前连续观测数", { exact: true }).fill("3");
  await workspace.getByLabel("上一观测最低成交量（手）", { exact: true }).fill("0");
  await workspace.getByLabel("上一观测最低持仓量（手）", { exact: true }).fill("100");
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  const task = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/rb/2026-10" });
  await expect(task.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await task.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "因子结果", exact: true });
  await result.getByText("每日资格与排除原因", { exact: true }).click();
  await result.getByLabel("资格日期", { exact: true }).fill(days[5]);
  const members = result.getByRole("table", { name: "每日成员资格", exact: true });
  await expect(members.getByRole("row").filter({ hasText: "SHFE/cu/2026-10" })).toContainText(
    "上一观测持仓量不足",
  );
  await expect(members.getByRole("row").filter({ hasText: "SHFE/rb/2026-10" })).toContainText(
    "有资格",
  );
  await expect(result.getByText("实际评价成员数", { exact: false })).toContainText("2");
  const state = await rpc(page.request, "runtime.snapshot", {});
  expect(state.task_result.experiment.liquidity).toEqual({
    minimum_history: 3,
    minimum_volume: "0",
    minimum_open_interest: "100",
  });
  expect(state.task_result.result.memberships).toHaveLength(days.length * 3);
  const id = state.task_result.id;
  await page.reload();
  const restored = await rpc(page.request, "task.result", { id });
  expect(restored.task_result.result.memberships).toEqual(state.task_result.result.memberships);
  expect(restored.task_result.experiment.liquidity).toEqual(state.task_result.experiment.liquidity);
});
