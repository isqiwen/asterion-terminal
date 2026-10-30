import { test, expect } from "./test";
import { restartServices, rpc, seedDataset, taskRpc, openResearch } from "./dataset-fixture";
import { openView } from "./navigation-helper";
test("independent fixed samples feed raw-close statistics without a factor and survive Data offline and Research restart", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  await seedDataset(page.request, [100, -1, 0, 2, -2, 4], "raw-sample-ui");
  await openView(page, "samples");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "固定当前数据选择", exact: true }).click();
  await expect(workspace.getByRole("region", { name: "固定样本来源", exact: true })).toContainText(
    "SHFE/rb/2026-10",
  );
  await workspace.getByLabel("样本用途", { exact: true }).fill("合成独立原价开发分布");
  await workspace.getByLabel("划分留出区间", { exact: true }).check();
  await workspace.getByLabel("首个留出观察索引", { exact: true }).fill("4");
  await workspace
    .getByLabel("确认仅作事后探索：历史可得时间未知，完整历史覆盖未获证明。", { exact: true })
    .check();
  await workspace.getByRole("button", { name: "提交或确认原样本任务", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const task = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (item: { kind: string }) => item.kind === "sample",
        );
        if (task) id = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const sample = (await taskRpc(page.request, "research", "task.result.read", { id })).task_result;
  expect(sample.experiment).toMatchObject({
    historical_availability: "unknown",
    complete_history: "unproven",
    request: { mode: "post_hoc", evaluation: { mode: "holdout", split_index: 4 } },
  });
  expect(sample.result.rows.map((row: { value: string | null }) => row.value)).toEqual([
    "100",
    "-1",
    "0",
    "2",
    null,
    null,
  ]);
  await workspace.getByRole("button", { name: "查看结果", exact: true }).click();
  const evidence = workspace.getByRole("region", { name: "正式样本证据", exact: true });
  const cells = evidence.getByRole("row");
  await expect(cells.nth(2).getByRole("cell").nth(4)).toHaveText("-1");
  await expect(cells.nth(3).getByRole("cell").nth(4)).toHaveText("0");
  await expect(cells.nth(5).getByRole("cell").nth(4)).toHaveText("—");
  await workspace.getByRole("button", { name: "提交或确认原样本任务", exact: true }).click();
  expect(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.filter(
      (task: { kind: string }) => task.kind === "sample",
    ),
  ).toHaveLength(1);
  const snapshot = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    service: snapshot.data.service,
    action: "stop",
  });
  await openView(page, "statistics");
  await workspace.getByRole("combobox", { name: "来源研究结果", exact: true }).selectOption(id);
  await workspace.getByRole("button", { name: "固定原结果", exact: true }).click();
  await expect(workspace.getByRole("combobox", { name: "统计列", exact: true })).toHaveValue(
    "raw_close",
  );
  await expect(
    workspace.getByRole("combobox", { name: "统计列", exact: true }).getByRole("option"),
  ).toHaveCount(1);
  await workspace.getByLabel("统计用途", { exact: true }).fill("保留原始零价和负价，不读取留出值");
  await workspace.getByLabel("直方图区间数", { exact: true }).fill("3");
  await workspace
    .getByRole("combobox", { name: "统计分组", exact: true })
    .selectOption("trading_day");
  await workspace.getByRole("button", { name: "提交或确认原统计任务", exact: true }).click();
  let statsId = "";
  await expect
    .poll(
      async () => {
        const task = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (item: { kind: string }) => item.kind === "statistics",
        );
        if (task) statsId = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const report = (await taskRpc(page.request, "research", "task.result.read", { id: statsId }))
    .task_result;
  expect(report.experiment.request).toMatchObject({
    source: { kind: "sample_result", id },
    column: "raw_close",
    role: "development",
  });
  expect(report.result.summary).toMatchObject({
    total: 6,
    allowed: 4,
    valid: 4,
    missing: 0,
    mean: { state: "DEFINED", value: 25.25 },
    minimum: { state: "DEFINED", value: -1 },
  });
  expect(report.result.price_power).toBe(1);
  expect(report.experiment.request.grouping).toBe("trading_day");
  expect(report.result.groups).toHaveLength(1);
  expect(report.result.groups[0]).toMatchObject({
    trading_day: "2026-09-25",
    summary: { total: 6, allowed: 4, valid: 4, mean: { state: "DEFINED", value: 25.25 } },
  });
  expect(report.result.rows[2]).toMatchObject({ reason: "VALID", value: 0 });
  expect(report.result.rows[4]).toMatchObject({ reason: "OTHER_ROLE", value: null, bin: null });
  expect(report.experiment.source.program_artifact).toBe(sample.task.program_artifact);
  await workspace.getByRole("button", { name: "查看结果", exact: true }).click();
  const formal = workspace.getByRole("region", { name: "正式统计报告", exact: true });
  await expect(formal.getByText("25.25", { exact: true })).toBeVisible();
  await expect(formal).toContainText("原始收盘价");
  await formal
    .getByRole("combobox", { name: "查看已保存交易日分组", exact: true })
    .selectOption("2026-09-25");
  await expect(
    formal
      .getByRole("table", { name: "统计逐行证据", exact: true })
      .getByRole("row")
      .nth(1)
      .getByRole("cell")
      .nth(5),
  ).toHaveText("2026-09-25");
  await expect(formal.getByText("25.25", { exact: true })).toBeVisible();
  expect(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.some(
      (task: { kind: string }) => task.kind === "factor",
    ),
  ).toBe(false);
  const research = snapshot.research_service.service;
  await restartServices(page.request, [research], () =>
    rpc(page.request, "node.action", { service: research, action: "restart" }),
  );
  await openResearch(page.request);
  expect((await taskRpc(page.request, "research", "task.result.read", { id })).task_result).toEqual(
    sample,
  );
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: statsId })).task_result,
  ).toEqual(report);
});
