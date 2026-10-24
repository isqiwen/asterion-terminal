import { expect, type Page } from "./test";
import { rpc, taskRpc, seedHistory } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import type { PortfolioStrategyPackage, TaskResult } from "../src/bridge/client";
import { readFileSync } from "node:fs";
type Evidence = Extract<TaskResult, { kind: "backtest" }>;

// One real new-conditions story; the separate partial-fill story retains its original contract.
export async function explicitPortfolioDelivery(
  page: Page,
  candidate: PortfolioStrategyPackage,
  source: Evidence,
  days: string[],
): Promise<{ id: string; delivery_id: string; result: Evidence }> {
  await openView(page, "strategies");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("button", { name: `${candidate.name} · ${candidate.object.id}`, exact: true })
    .click();
  await workspace.getByRole("button", { name: "查询原草稿与接纳状态", exact: true }).click();
  await workspace.getByRole("button", { name: "另建独立交付草稿", exact: true }).click();
  const target = `new-${source.id}`;
  await workspace.getByLabel("目标回测任务ID", { exact: true }).fill(target);
  await workspace.getByRole("button", { name: "保存交付草稿，不运行", exact: true }).click();
  await expect(workspace.getByText("草稿已保存，尚无正式接纳", { exact: true })).toBeVisible();
  const before = (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  expect(before).toHaveLength(2);
  const drafts = (
    await rpc(page.request, "research.strategy.delivery.list", {
      package_id: candidate.object.id,
      after: "",
      limit: 30,
    })
  ).research_strategy.deliveries;
  const draft = drafts.find((d: { target_task_id: string }) => d.target_task_id === target);
  expect(draft).toBeTruthy();
  const one = await seedHistory(
    page.request,
    days.map(() => 1000),
    "new-portfolio-rb",
    { tablesOnly: true, dailyPrices: true, dailyDays: days, openInterest: 101 },
  );
  const two = await seedHistory(
    page.request,
    days.map(() => 1000),
    "new-portfolio-hc",
    {
      product: "hc",
      keep: true,
      tablesOnly: true,
      dailyPrices: true,
      dailyDays: days,
      openInterest: 101,
    },
  );
  for (const [product, table] of [
    ["rb", one],
    ["hc", two],
  ] as const)
    await rpc(page.request, "data.dataset.daily", {
      contract_id: `SHFE/${product}/2026-10`,
      daily: table.daily,
      begin_day: days[0],
      end_day: days.at(-1)!,
    });
  await workspace.getByRole("button", { name: "按固定组合新配置回测", exact: true }).click();
  const delivery = page.getByRole("region", { name: "固定组合交付回测", exact: true });
  await expect(delivery).toContainText(candidate.name);
  expect((await rpc(page.request, "runtime.snapshot")).research_service.tasks).toHaveLength(2);
  await page.getByRole("button", { name: "下一步", exact: true }).click();
  const policy = page.getByLabel("使用固定权重组合政策（仅完整普通日线）", { exact: true });
  await expect(policy).toBeChecked();
  await expect(policy).toBeDisabled();
  for (const [label, value] of [
    ["原合约名义权重 SHFE/rb2610", "0.4"],
    ["原合约名义权重 SHFE/hc2610", "-0.2"],
    ["组合总绝对名义权重预算", "0.8"],
    ["组合净名义权重预算", "0.3"],
  ]) {
    await expect(page.getByLabel(label, { exact: true })).toHaveValue(value);
    await expect(page.getByLabel(label, { exact: true })).toBeDisabled();
  }
  for (const [label, value] of [
    ["初始资金", "100000"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await page.getByLabel(label, { exact: true }).fill(value);
  for (const symbol of ["rb2610", "hc2610"]) {
    const costs = page.getByRole("region", {
      name: `SHFE · ${symbol} 保证金与手续费`,
      exact: true,
    });
    for (const [label, value] of [
      ["每手保证金", "100"],
      ["滑点（跳）", "0"],
      ["每手开仓费", "4"],
      ["每手平今费", "6"],
      ["每手平昨费", "8"],
    ])
      await costs.getByLabel(label, { exact: true }).fill(value);
  }
  await page.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(page.getByText("固定组合名义政策", { exact: true })).toBeVisible();
  expect((await rpc(page.request, "runtime.snapshot")).research_service.tasks).toHaveLength(2);
  await page.getByRole("button", { name: "开始回测", exact: true }).click();
  await page
    .getByRole("region", { name: "回测进度", exact: true })
    .getByRole("button", { name: "查看结果", exact: true })
    .click({ timeout: 30000 });
  const result = (await taskRpc(page.request, "research", "task.result.read", { id: target }))
    .task_result;
  expect(result.kind).toBe("backtest");
  expect(result.result.account.fees).toBe("40");
  expect(result.result.account.balance).toBe("99960");
  expect(result.task.strategy_use.package).toEqual(candidate.object);
  expect(result.task.strategy_use.delivery).toEqual(draft.object);
  expect(result.task.strategy_use.request_sha256).toMatch(/^[0-9a-f]{64}$/);
  expect(result.task.program_artifact).toBe(candidate.definition.program_artifact);
  expect(result.input_sha256).not.toBe(source.input_sha256);
  for (const [c, table] of [
    [0, one],
    [1, two],
  ] as const) {
    expect(result.experiment.paper.contracts[c].dataset.daily.snapshot).toBe(table.daily.snapshot);
    expect(result.experiment.paper.contracts[c].dataset.daily.snapshot).not.toBe(
      source.experiment.paper.contracts[c].dataset.daily.snapshot,
    );
  }
  const original = (await taskRpc(page.request, "research", "task.result.read", { id: source.id }))
    .task_result;
  expect(original.result).toEqual(source.result);
  expect(original.task.result_digest).toBe(source.task.result_digest);
  const downloading = page.waitForEvent("download");
  await page.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const download = await downloading;
  const path = await download.path();
  expect(path).toBeTruthy();
  const document = JSON.parse(readFileSync(path!, "utf8"));
  expect(document.source.task_id).toBe(target);
  expect(document.source.program_artifact).toBe(result.task.program_artifact);
  expect(document.strategy_use).toEqual(result.task.strategy_use);
  expect(document.result).toEqual(result.result);
  expect(
    (await rpc(page.request, "research.strategy.read", { id: candidate.object.id }))
      .research_strategy.package,
  ).toEqual(candidate);
  return { id: target, delivery_id: draft.object.id, result };
}
