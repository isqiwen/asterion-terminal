import { taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { readFile } from "node:fs/promises";
import { rpc, seedHistory } from "./dataset-fixture";
import { test, expect } from "./test";
import { checkSnapshot } from "./snapshot-schema";

// Daily bars are the market-wide daily table's own: nothing is downloaded or
// chosen for them. This journey has a node of its own; the dominant series
// spec ends with a second task store deployed on its node.
test("a contract's days and a product's months backtest from the daily table", async ({ page }) => {
  test.setTimeout(60000);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // The series reads every month the daily table has of the product. ag2610
  // holds the open interest until ag2701 appears with more of it, settling at
  // twice the price: the roll falls on 2026-09-25.
  await seedHistory(page.request, [100], "daily-near", {
    product: "ag",
    minuteDays: ["2026-09-25"],
    dailyDays: ["2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25"],
  });
  await seedHistory(page.request, [100], "daily-far", {
    product: "ag",
    keep: true,
    month: "2027-01",
    minuteDays: ["2026-09-25"],
    dailyDays: ["2026-09-24", "2026-09-25", "2026-09-28"],
    openInterest: 200,
    settlement: 220,
  });
  await page.reload();
  await openView(page, "backtest");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
  const selected = workspace.getByRole("list", { name: "已选合约" });
  // One contract: the days it traded are its bars, and nothing is downloaded
  // or chosen for it.
  const contract = workspace.getByRole("form", { name: "日线合约", exact: true });
  await contract.getByLabel("合约", { exact: true }).fill("SHFE/ag/2026-10");
  await contract.getByLabel("开始交易日", { exact: true }).fill("2026-09-21");
  await contract.getByLabel("结束交易日", { exact: true }).fill("2026-09-25");
  await contract.getByRole("button", { name: "使用这个合约的日线", exact: true }).click();
  await expect(selected).toContainText("SHFE · ag2610 · 日线 · 5 根 · 5 个交易日");
  await expect(selected).toContainText("日线和结算价取自全市场日线表");
  const one = (await rpc(page.request, "runtime.snapshot")).datasets;
  expect(one).toHaveLength(1);
  expect(one[0].source_dataset_ids).toEqual([]);
  expect(one[0].interval_minutes).toBe(0);
  await selected.getByRole("button", { name: "移除 SHFE · ag2610", exact: true }).click();
  await expect(selected.locator("li")).toHaveCount(0);
  // The product: the data service finds the months that traded in the range.
  await workspace.getByRole("button", { name: "日线主力连续", exact: true }).click();
  const series = workspace.getByRole("form", { name: "日线主力连续", exact: true });
  await series.getByLabel("品种", { exact: true }).fill("SHFE/ag");
  await series.getByLabel("开始交易日", { exact: true }).fill("2026-09-21");
  await series.getByLabel("结束交易日", { exact: true }).fill("2026-09-28");
  await series.getByRole("button", { name: "使用这个品种的日线主力连续", exact: true }).click();
  await expect(selected).toContainText("SHFE · ag 主力连续");
  // ag2610 from the second day to the roll day, ag2701 from the day before it
  // takes over.
  await expect(selected).toContainText("日线 · 7 根 · 2 个月份");
  await selected.getByText("换月日程（1 次换月）", { exact: true }).click();
  const rolls = selected.getByRole("table", { name: "SHFE · ag 主力连续 换月日程" });
  await expect(rolls.locator("tbody tr")).toHaveText([
    /2026-09-22\s*ag2610\s*2/,
    /2026-09-25\s*ag2701\s*1/,
  ]);
  const state = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("daily dominant series selection", state);
  expect(state.dataset_series).toHaveLength(1);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(
    workspace.getByRole("region", { name: "SHFE · ag 主力连续 保证金与手续费" }),
  ).toBeVisible();
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["每手保证金", "100"],
    ["滑点（跳）", "0"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "1"],
    ["总持仓量上限", "1"],
    ["在途委托数上限", "2"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  await expect(
    workspace.getByText("任务已提交，可在任务中心查看进度。", { exact: true }),
  ).toBeVisible();
  await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
  const row = workspace.getByRole("region", { name: "研究任务", exact: true }).locator("tbody tr");
  await expect(row.first().getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  // The task froze the months the table named on the day it was submitted:
  // both are read from the daily table, and every day of the series settles.
  const task = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.at(-1);
  const evidence = (await taskRpc(page.request, "research", "task.result", { id: task.id }))
    .task_result;
  expect(
    evidence.experiment.paper.contracts.map(
      (item: { dataset: { contract: { symbol: string }; source_dataset_ids: string[] } }) => [
        item.dataset.contract.symbol,
        item.dataset.source_dataset_ids,
      ],
    ),
  ).toEqual([
    ["ag2610", []],
    ["ag2701", []],
  ]);
  expect(evidence.result.settlements).toHaveLength(5);
  // The details say where bars without an archive version came from.
  await row.first().getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "回测结果", exact: true });
  await result.getByText("实验参数", { exact: true }).click();
  // The months are one block, with the costs they all trade at.
  const details = result.getByRole("region", { name: "SHFE · ag · 主力连续", exact: true });
  await expect(details).toContainText("ag2610 – ag2701（2 个）");
  await expect(details).toContainText("取自全市场日线表");
  await expect(result.getByRole("region", { name: "SHFE · ag2610", exact: true })).toHaveCount(0);
  const panel = result.getByRole("region", { name: "按此回测创建", exact: true });
  const create = panel.getByRole("button", { name: "创建独立回测任务", exact: true });
  await expect(create).toBeDisabled();
  await panel.getByLabel("沿用原固定快照", { exact: true }).check();
  await create.click();
  const fixedId = await panel.getByRole("status").locator("code").textContent();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (item: { id: string }) => item.id === fixedId,
        )?.state,
    )
    .toBe("succeeded");
  const fixed = (await taskRpc(page.request, "research", "task.result", { id: fixedId }))
    .task_result;
  expect(fixed.input_sha256).toBe(evidence.input_sha256);
  expect(fixed.experiment).toEqual(evidence.experiment);
  expect(fixed.result).toEqual(evidence.result);
  await seedHistory(page.request, [100], "daily-far-revised", {
    product: "ag",
    keep: true,
    tablesOnly: true,
    month: "2027-01",
    dailyDays: ["2026-09-24", "2026-09-25", "2026-09-28"],
    openInterest: 200,
    settlement: 230,
  });
  await page.reload();
  await taskRpc(page.request, "research", "task.result", { id: task.id });
  await openView(page, "backtest");
  const originalRow = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ has: page.getByText(task.id, { exact: true }) });
  await originalRow.getByRole("button", { name: "查看结果", exact: true }).click();
  await panel.getByLabel("选择当前服务的新快照", { exact: true }).check();
  await expect(create).toBeEnabled();
  await create.click();
  const revisedId = await panel.getByRole("status").locator("code").textContent();
  expect(revisedId).not.toBe(fixedId);
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (item: { id: string }) => item.id === revisedId,
        )?.state,
    )
    .toBe("succeeded");
  const revised = (await taskRpc(page.request, "research", "task.result", { id: revisedId }))
    .task_result;
  expect(revised.input_sha256).not.toBe(evidence.input_sha256);
  expect(revised.experiment.dataset_revision).not.toBe(evidence.experiment.dataset_revision);
  expect(revised.experiment.strategies).toEqual(evidence.experiment.strategies);
  expect(revised.experiment.paper.risk).toEqual(evidence.experiment.paper.risk);
  expect(revised.result).not.toEqual(evidence.result);
  expect(
    (await taskRpc(page.request, "research", "task.result", { id: task.id })).task_result,
  ).toEqual(evidence);
  // Comparison is its own view, outside the research workspace.
  await openView(page, "validation");
  const comparison = page.getByRole("region", { name: "回测比较", exact: true });
  for (const id of [task.id, fixedId, revisedId])
    await comparison.getByLabel(id + " · " + evidence.task.instrument, { exact: true }).check();
  const selectedBefore = (await rpc(page.request, "runtime.snapshot")).task_result;
  await comparison.getByRole("button", { name: "读取并比较", exact: true }).click();
  const matrix = comparison.getByRole("table", { name: "回测实验对照", exact: true });
  await expect(matrix).toBeVisible();
  const method = matrix
    .getByRole("row")
    .filter({ has: page.getByRole("rowheader", { name: "比较口径", exact: true }) });
  await expect(method).toContainText("同口径对照");
  await expect(method).toContainText("仅独立列示");
  await expect(method).toContainText("固定快照不同");
  await expect(matrix).toContainText(evidence.experiment.paper.contracts[0].dataset.daily.snapshot);
  await expect(matrix).toContainText(revised.experiment.paper.contracts[0].dataset.daily.snapshot);
  await expect(
    matrix.getByRole("rowheader", { name: "逐日费用与保证金", exact: true }),
  ).toBeVisible();
  await expect(matrix.getByRole("rowheader", { name: "换月日程", exact: true })).toBeVisible();
  const visibleMatrixRows = await matrix.evaluate(element => {
    const table = element as HTMLTableElement;
    return Array.from(table.tBodies[0].rows).map(row => ({
      label: row.cells[0].textContent?.trim() ?? "",
      cells: Array.from(row.cells)
        .slice(1)
        .map(cell => ({
          text: cell.textContent?.trim() ?? "",
          state: cell.dataset.valueState ?? "display_only",
          metrics: Array.from(cell.querySelectorAll<HTMLElement>("dd[data-value-state]")).map(
            value => ({
              label: value.previousElementSibling?.textContent?.trim() ?? "",
              text: value.textContent?.trim() ?? "",
              state: value.dataset.valueState!,
            }),
          ),
        })),
    }));
  });
  const comparisonExport = comparison.getByRole("region", { name: "比较证据导出", exact: true });
  const comparisonDownloadEvent = page.waitForEvent("download");
  await comparisonExport.getByRole("button", { name: "导出当前比较证据", exact: true }).click();
  const comparisonDownload = await comparisonDownloadEvent;
  const comparisonBytes = await readFile((await comparisonDownload.path())!);
  const comparisonDocument = JSON.parse(comparisonBytes.toString("utf8"));
  expect(comparisonDocument).toMatchObject({
    format: "asterion.research.comparison.evidence",
  });
  expect(comparisonDocument.matrix.columns.map((column: { id: string }) => column.id)).toEqual([
    task.id,
    fixedId,
    revisedId,
  ]);
  expect(comparisonDocument.matrix.rows).toEqual(visibleMatrixRows);
  expect(comparisonDocument.matrix.columns[2].methodology).toBe("independent");
  expect(comparisonDocument.matrix.columns[2].reasons).toContain("固定快照不同");
  const methodRow = comparisonDocument.matrix.rows.find(
    (row: { label: string }) => row.label === "比较口径",
  );
  for (const reason of comparisonDocument.matrix.columns[2].reasons)
    expect(methodRow.cells[2].text).toContain(reason);
  const originals = [evidence, fixed, revised];
  comparisonDocument.experiments.forEach(
    (member: { experiment: unknown; result: unknown }, i: number) => {
      expect(member.experiment).toEqual(originals[i].experiment);
      expect(member.result).toEqual(originals[i].result);
    },
  );
  await expect(comparisonExport.getByRole("status")).toContainText(
    `${comparisonBytes.length} bytes`,
  );

  expect((await rpc(page.request, "runtime.snapshot")).task_result).toEqual(selectedBefore);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: task.id })).task_result,
  ).toEqual(evidence);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: revisedId })).task_result,
  ).toEqual(revised);
  expect((await rpc(page.request, "runtime.snapshot")).task_result).toEqual(selectedBefore);
  // Back to the result the comparison left; the view keeps where it was.
  await openView(page, "backtest");
  const exportPanel = workspace.getByRole("region", { name: "研究证据导出", exact: true });
  const downloadEvent = page.waitForEvent("download");
  await exportPanel.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const download = await downloadEvent;
  const bytes = await readFile((await download.path())!);
  const document = JSON.parse(bytes.toString("utf8"));
  expect(document).toMatchObject({
    format: "asterion.research.evidence",
    source: {
      task_id: task.id,
      kind: "backtest",
      input_sha256: evidence.input_sha256,
      result_sha256: evidence.task.result_digest,
    },
  });
  expect(document.experiment).toEqual(evidence.experiment);
  expect(document.result).toEqual(evidence.result);
  expect(document.performance).toEqual(evidence.performance);
  expect(document.trades).toEqual(evidence.trades);
  await expect(exportPanel.getByRole("status")).toContainText(`${bytes.length} bytes`);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: task.id })).task_result,
  ).toEqual(evidence);
  const missing = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "task.evidence.export",
      params: { id: "missing-export", path: "" },
    },
  });
  expect((await missing.json()).error).toBeTruthy();
  expect(fixed.task.metadata).toMatchObject({
    source_id: task.id,
    source_result_digest: evidence.task.result_digest,
    data_choice: "fixed",
    snapshot: "",
  });
  expect(revised.task.metadata).toMatchObject({
    source_id: task.id,
    data_choice: "snapshot",
    snapshot: revised.experiment.paper.contracts[0].dataset.daily.snapshot,
  });
});

// The market chart reads a contract's days from the daily table through the
// real core and data service: nothing is downloaded for them.
test("the market chart draws a contract's days from the daily table", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // Forty weekdays, eight whole weeks, of lead's October month, with no
  // minute download or live CTP catalog entry.
  const days: string[] = [];
  for (let at = Date.UTC(2026, 7, 17); days.length < 40; at += 86400000)
    if (new Date(at).getUTCDay() % 6 !== 0) days.push(new Date(at).toISOString().slice(0, 10));
  const { daily } = await seedHistory(page.request, [100], "chart-pb", {
    product: "pb",
    tablesOnly: true,
    dailyDays: days,
  });
  const archives = await rpc(page.request, "data.datasets", {
    venue: "",
    product: "",
    contract_id: "SHFE/pb/2026-10",
    source: "",
  });
  expect(archives.history_datasets).toEqual([]);
  const weeks = await rpc(page.request, "data.tables.daily", {
    daily,
    contract: "SHFE/pb/2026-10",
    period: "week",
    offset: 0,
    limit: 100,
  });
  checkSnapshot("daily table chart", weeks);
  expect(weeks.table_daily!.total).toBe(8);
  expect(weeks.table_daily!.candles[0]).toEqual({
    trading_day: "2026-08-21",
    open: "100",
    high: "201",
    low: "90",
    close: "110",
    volume: "50000",
  });
  await page.reload();
  await openView(page, "market");
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await page.getByRole("button", { name: "日线合约", exact: true }).click();
  const board = page.locator(".history-market-board");
  await board.getByRole("combobox", { name: "交易所", exact: true }).selectOption("SHFE");
  await board.getByRole("combobox", { name: "品种", exact: true }).selectOption("pb");
  await board
    .getByRole("combobox", { name: "合约月份", exact: true })
    .selectOption("SHFE/pb/2026-10");
  await expect(board.locator(".market-history-pages")).toContainText("1–40 / 40");
  await expect(board.locator(".market-chart-source")).toContainText("全市场日线表");
  await board.getByRole("button", { name: "周线", exact: true }).click();
  await expect(board.locator(".market-history-pages")).toContainText("1–8 / 8");
});
