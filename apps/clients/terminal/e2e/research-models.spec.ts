import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
test("fixed model page trains only mature original rows and reads exact predictions after restart without Data", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  await seedDataset(
    page.request,
    Array.from({ length: 90 }, (_, i) => 100 + i + (i % 3)),
    "model-ui-data",
  );
  await rpc(page.request, "factor.submit", {
    id: "model-ui-source",
    series: { kind: "bars" },
    factor: "momentum",
    lookbacks: [2],
    horizon: 2,
    evaluation: { mode: "holdout", split_index: 45 },
    retain_observations: true,
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (value: { id: string }) => value.id === "model-ui-source",
        )?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const source = (
    await taskRpc(page.request, "research", "task.result.read", { id: "model-ui-source" })
  ).task_result;
  await openView(page, "models");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(
    workspace.getByRole("heading", { name: "固定来源模型训练", exact: true }),
  ).toBeVisible();
  await workspace.getByLabel("模型名称", { exact: true }).fill("固定单特征模型");
  await workspace.getByLabel("模型用途", { exact: true }).fill("合成隔离数据上的事后诊断");
  await workspace
    .getByLabel("模型限制说明", { exact: true })
    .fill("未知历史可得时间，不是独立留出验证或交易授权");
  await workspace.getByLabel("原因子任务ID", { exact: true }).fill("model-ui-source");
  await workspace.getByRole("button", { name: "读取并固定原因子证据", exact: true }).click();
  await expect(workspace.getByText(new RegExp(source.task.result_digest))).toBeVisible();
  await workspace.getByRole("combobox", { name: "回归方法", exact: true }).selectOption("linear");
  await workspace.getByLabel("训练次数预算", { exact: true }).fill("1");
  await workspace.getByLabel("训练时间预算（秒）", { exact: true }).fill("30");
  await workspace.getByLabel("训练截止索引（不含）", { exact: true }).fill("12");
  await workspace.getByLabel("训练样本标准化（n−1）", { exact: true }).check();
  await workspace.getByLabel("适用截止（UTC）", { exact: true }).fill("2100-01-01T00:00");
  await workspace.getByRole("button", { name: "保存模型草稿", exact: true }).click();
  await expect(workspace.getByText(/已保存原草稿/)).toBeVisible();
  expect(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.filter(
      (value: { kind: string }) => value.kind === "model_training",
    ),
  ).toHaveLength(0);
  await workspace.getByRole("button", { name: "固定模型训练方案", exact: true }).click();
  const fixed = workspace.getByRole("region", { name: "固定模型训练方案", exact: true });
  await expect(fixed.getByText(/实际成熟训练成员.*8/)).toBeVisible();
  const plan = (
    await rpc(page.request, "research.model.list", { kind: "plan", after: "", limit: 30 })
  ).research_model.plans.find(
    (value: { specification: { name: string } }) => value.specification.name === "固定单特征模型",
  );
  expect(plan.specification.standardize).toBe(true);
  expect(plan.source.result.version).toBe(source.task.result_digest);
  expect(plan.source.program_artifact).toBe(source.task.program_artifact);
  const state = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    id: "local",
    service: state.data.service,
    action: "stop",
  });
  await workspace.getByLabel("模型训练任务ID", { exact: true }).fill("model-ui-training");
  await workspace.getByRole("button", { name: "显式接纳训练任务", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (value: { id: string }) => value.id === "model-ui-training",
        )?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const original = (
    await taskRpc(page.request, "research", "task.result.read", { id: "model-ui-training" })
  ).task_result;
  expect(original.kind).toBe("model_training");
  expect(original.experiment.plan.object).toEqual(plan.object);
  expect(original.result.training_samples).toBe(8);
  expect(original.result.standardization.training_samples).toBe(8);
  expect(original.result.standardization.sample_standard_deviation).toBeGreaterThan(0);
  expect(original.result.rows).toHaveLength(90);
  expect(original.result.rows[0].prediction).toBeNull();
  expect(original.result.rows[10].reason).toBe("crossed_label");
  expect(original.result.rows[11].reason).toBe("crossed_label");
  expect(original.result.rows[89].prediction_state).toBe("predicted");
  await workspace.getByRole("button", { name: "读取原模型结果", exact: true }).click();
  const report = workspace.getByRole("region", { name: "原模型训练结果", exact: true });
  await expect(report.getByText(/固定训练均值与样本尺度/)).toBeVisible();
  await expect(report.getByRole("row").nth(1).getByRole("cell").nth(3)).toHaveText("不可用");
  await expect(report.getByRole("row").nth(3).getByRole("cell").nth(3)).toHaveText(
    String(original.result.rows[2].prediction),
  );
  await rpc(page.request, "research.model.train", { id: "model-ui-training", plan: plan.object });
  const unchanged = (
    await taskRpc(page.request, "research", "task.result.read", { id: "model-ui-training" })
  ).task_result;
  expect(unchanged).toEqual(original);
  await rpc(page.request, "node.action", {
    id: "local",
    service: state.research_service.service,
    action: "restart",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).research_service?.online, {
      timeout: 15000,
    })
    .toBe(true);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "model-ui-training" }))
      .task_result,
  ).toEqual(original);
  expect(
    (await rpc(page.request, "research.model.read", { kind: "plan", id: plan.object.id }))
      .research_model.plan,
  ).toEqual(plan);
  await page.reload();
  await openView(page, "models");
  await workspace
    .getByRole("button", { name: `固定单特征模型 · ${plan.object.id}`, exact: true })
    .click();
  await expect(workspace.getByLabel("模型训练任务ID", { exact: true })).toHaveValue(
    "model-ui-training",
  );
  await workspace.getByRole("button", { name: "读取原模型结果", exact: true }).click();
  await expect(report.getByRole("row").nth(3).getByRole("cell").nth(3)).toHaveText(
    String(original.result.rows[2].prediction),
  );

  const mappingPanel = workspace.getByRole("region", { name: "固定预测映射", exact: true });
  await mappingPanel.getByLabel("映射名称", { exact: true }).fill("原模型诊断方向");
  await mappingPanel
    .getByLabel("映射用途", { exact: true })
    .fill("合成固定模型预测解释，不作历史交易证据");
  await mappingPanel.getByLabel("做空阈值（收益比率）", { exact: true }).fill("-0.01");
  await mappingPanel.getByLabel("做多阈值（收益比率）", { exact: true }).fill("0.01");
  await mappingPanel.getByLabel("中间区间无新方向意图，不表示平仓", { exact: true }).check();
  await mappingPanel.getByLabel("映射适用截止（UTC）", { exact: true }).fill("2099-01-01T00:00");
  await mappingPanel.getByRole("button", { name: "固定预测映射", exact: true }).click();
  await expect(
    mappingPanel.getByRole("region", { name: "已固定预测契约", exact: true }),
  ).toBeVisible();
  const mapping = (
    await rpc(page.request, "research.model.list", { kind: "mapping", after: "", limit: 10 })
  ).research_model.mappings[0];
  expect(mapping.specification.model.id).toBe(original.id);
  expect(mapping.specification.model.version).toBe(original.task.result_digest);
  await mappingPanel.getByRole("button", { name: "读取原行诊断映射", exact: true }).click();
  const mapped = mappingPanel.getByRole("region", { name: "原行诊断映射", exact: true });
  await expect(mapped.getByRole("row").nth(1).getByRole("cell").nth(2)).toHaveText("不可用");
  await expect(mapped.getByRole("row").nth(1).getByRole("cell").nth(3)).toHaveText("预测不可用");
  await expect(mapped.getByRole("row").nth(3).getByRole("cell").nth(2)).toHaveText(
    String(original.result.rows[2].prediction),
  );
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "model-ui-training" }))
      .task_result,
  ).toEqual(original);

  const timePanel = workspace.getByRole("region", { name: "逐折模型时间验证", exact: true });
  await timePanel.getByRole("button", { name: "从当前固定方案建立时间验证", exact: true }).click();
  await timePanel.getByLabel("时间验证名称", { exact: true }).fill("原方案逐折开发验证");
  await timePanel
    .getByLabel("时间验证用途", { exact: true })
    .fill("合成数据真实成熟标签和折内尺度");
  await timePanel
    .getByLabel("时间验证限制说明", { exact: true })
    .fill("历史可得时间未知，原留出已观察，不作交易证据");
  await timePanel.getByRole("spinbutton", { name: "验证起始索引 1", exact: true }).fill("12");
  await timePanel.getByRole("spinbutton", { name: "验证截止索引 1", exact: true }).fill("20");
  await timePanel.getByRole("button", { name: "添加显式折", exact: true }).click();
  for (const [label, value] of [
    ["训练起始索引 2", "8"],
    ["训练截止索引 2", "20"],
    ["验证起始索引 2", "20"],
    ["验证截止索引 2", "28"],
  ])
    await timePanel.getByRole("spinbutton", { name: label, exact: true }).fill(value);
  await timePanel.getByLabel("逐折拟合次数预算", { exact: true }).fill("2");
  await timePanel.getByLabel("逐折任务时间预算（秒）", { exact: true }).fill("30");
  await timePanel.getByLabel("时间验证截止（UTC）", { exact: true }).fill("2099-01-01T00:00");
  await timePanel.getByRole("button", { name: "固定逐折计划（不执行）", exact: true }).click();
  const timeConfirmation = timePanel.getByRole("region", { name: "固定逐折确认", exact: true });
  await expect(timeConfirmation).toBeVisible();
  await expect(timeConfirmation.getByRole("row").nth(1).getByRole("cell").nth(3)).toHaveText("8");
  await expect(timeConfirmation.getByRole("row").nth(2).getByRole("cell").nth(3)).toHaveText("10");
  const timePlan = (
    await rpc(page.request, "research.model.list", { kind: "time_plan", after: "", limit: 10 })
  ).research_model.time_plans[0];
  expect(timePlan.specification.recipe).toEqual(plan.object);
  expect(timePlan.boundaries.map((value: { event_index: number }) => value.event_index)).toEqual([
    0, 12, 12, 20, 8, 20, 20, 28,
  ]);
  for (const value of timePlan.boundaries) {
    expect(typeof value.timestamp_ns).toBe("string");
    await expect(
      timeConfirmation.getByText(`${value.timestamp_ns} ns`, { exact: false }).first(),
    ).toBeVisible();
    await expect(
      timeConfirmation.getByText(`原交易日: ${value.trading_day}`, { exact: false }).first(),
    ).toBeVisible();
  }
  await timePanel.getByLabel("逐折任务ID", { exact: true }).fill("model-time-ui");
  await timePanel.getByRole("button", { name: "显式接纳逐折任务", exact: true }).click();
  await expect(timePanel.getByRole("button", { name: "读取原逐折报告", exact: true })).toBeEnabled({
    timeout: 30000,
  });
  await timePanel.getByRole("button", { name: "读取原逐折报告", exact: true }).click();
  const timeReport = timePanel.getByRole("region", { name: "原逐折模型报告", exact: true });
  const timeOriginal = (
    await taskRpc(page.request, "research", "task.result.read", { id: "model-time-ui" })
  ).task_result;
  expect(timeOriginal.kind).toBe("model_time_validation");
  expect(timeOriginal.result.scope).toBe("development_validation");
  expect(timeOriginal.result.historical_availability_unknown).toBe(true);
  expect(timeOriginal.result.folds).toHaveLength(2);
  expect(timeOriginal.result.folds[0].training_members).toEqual([2, 3, 4, 5, 6, 7, 8, 9]);
  expect(timeOriginal.result.folds[0].standardization).toEqual(original.result.standardization);
  expect(timeOriginal.result.folds[0].coefficient).toBe(original.result.coefficient);
  expect(timeOriginal.result.folds[0].predictions[7].exclusion).toBe("crossed_label");
  expect(timeOriginal.result.folds[0].predictions[7].prediction).not.toBeNull();
  await expect(timeReport.getByRole("row").nth(8).getByRole("cell").nth(1)).toHaveText(
    String(timeOriginal.result.folds[0].predictions[7].prediction),
  );
  await expect(timeReport.getByRole("row").nth(8).getByRole("cell").nth(2)).toHaveText(
    "标签跨越验证截止",
  );
  const foldSelector = timeReport.getByRole("combobox", { name: "查看原折", exact: true });
  await foldSelector.selectOption({ value: "1" });
  await expect(foldSelector).toHaveValue("1");
  await expect(timeReport.getByRole("row").nth(1).getByRole("cell").nth(0)).toHaveText("20");
  const timeMappingPanel = timePanel.getByRole("region", { name: "逐折固定预测映射", exact: true });
  await timeMappingPanel.getByLabel("映射名称", { exact: true }).fill("逐折开发方向");
  await timeMappingPanel
    .getByLabel("映射用途", { exact: true })
    .fill("原逐折预测的诊断意图，不是历史可交易信号");
  await timeMappingPanel.getByLabel("做空阈值（收益比率）", { exact: true }).fill("-0.01");
  await timeMappingPanel.getByLabel("做多阈值（收益比率）", { exact: true }).fill("0.01");
  await timeMappingPanel.getByLabel("中间区间无新方向意图，不表示平仓", { exact: true }).check();
  await timeMappingPanel
    .getByLabel("映射适用截止（UTC）", { exact: true })
    .fill("2099-01-01T00:00");
  await timeMappingPanel.getByRole("button", { name: "逐折固定预测映射", exact: true }).click();
  await expect(
    timeMappingPanel.getByRole("region", { name: "已固定预测契约", exact: true }),
  ).toBeVisible();
  const timeMappings = (
    await rpc(page.request, "research.model.list", { kind: "mapping", after: "", limit: 10 })
  ).research_model.mappings;
  const timeMapping = timeMappings.find(
    (value: { specification: { model: { id: string } } }) =>
      value.specification.model.id === "model-time-ui",
  );
  expect(timeMapping.specification.model.version).toBe(timeOriginal.task.result_digest);
  expect(timeMapping.plan).toEqual(timePlan.object);
  await timeMappingPanel.getByRole("button", { name: "读取原行诊断映射", exact: true }).click();
  const timeMapped = timeMappingPanel.getByRole("region", { name: "原行诊断映射", exact: true });
  await expect(timeMapped.getByRole("row")).toHaveCount(17);
  await expect(timeMapped.getByRole("row").nth(9).getByRole("cell").nth(0)).toHaveText("20");
  await expect(timeMapped.getByRole("row").nth(8).getByRole("cell").nth(1)).toContainText(
    "标签跨越验证截止",
  );
  await expect(timeMapped.getByRole("row").nth(8).getByRole("cell").nth(2)).toHaveText(
    String(timeOriginal.result.folds[0].predictions[7].prediction),
  );

  await rpc(page.request, "node.action", {
    id: "local",
    service: state.research_service.service,
    action: "restart",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).research_service?.online, {
      timeout: 15000,
    })
    .toBe(true);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "model-time-ui" }))
      .task_result,
  ).toEqual(timeOriginal);
  expect(
    (await rpc(page.request, "research.model.read", { kind: "time_plan", id: timePlan.object.id }))
      .research_model.time_plan,
  ).toEqual(timePlan);

  await page.goto("/?screen=settings&category=resources");
  const catalogue = page.getByRole("region", { name: "业务资产目录", exact: true });
  await catalogue
    .getByRole("combobox", { name: "资产所属域", exact: true })
    .selectOption("research");
  for (const [kind, id] of [
    ["model_draft", plan.draft_id],
    ["model_plan", plan.object.id],
    ["model_mapping", mapping.object.id],
    ["model_mapping", timeMapping.object.id],
    ["model_time_plan", timePlan.object.id],
    ["model_time_validation", "model-time-ui"],
    ["model_time_validation_result", "model-time-ui"],
    ["model_training", "model-ui-training"],
    ["model_training_result", "model-ui-training"],
  ]) {
    await catalogue.getByRole("combobox", { name: "资产类别", exact: true }).selectOption(kind);
    await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
    await expect(catalogue.getByText(id, { exact: true })).toBeVisible();
    await expect(
      catalogue.getByRole("button", { name: "查看引用与维护", exact: true }),
    ).toHaveCount(0);
  }
  await rpc(page.request, "node.action", {
    id: "local",
    service: state.research_service.service,
    action: "stop",
  });
  const selected = [state.data.service, state.research_service.service];
  const stopped = async () =>
    (await rpc(page.request, "runtime.snapshot")).nodes
      .find((node: { id: string }) => node.id === "local")
      .health.services.filter((item: { id: string }) => selected.includes(item.id))
      .every(
        (item: { desired_running: boolean; pid: number }) => !item.desired_running && !item.pid,
      );
  await expect.poll(stopped, { timeout: 10000 }).toBe(true);
  const temporary = mkdtempSync(join(tmpdir(), "asterion-model-cold-browser-"));
  try {
    const backup = join(temporary, "backup"),
      restored = join(temporary, "restored");
    await page.goto("/?screen=settings&category=backup");
    const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    for (const id of selected)
      await maintenance.getByRole("checkbox", { name: new RegExp(id) }).check();
    await maintenance.getByLabel("新备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "预览备份", exact: true }).click();
    await expect(maintenance.getByRole("button", { name: "保存备份", exact: true })).toBeDisabled();
    await maintenance
      .getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录", exact: true })
      .check();
    await maintenance.getByRole("button", { name: "保存备份", exact: true }).click();
    await expect(maintenance).toContainText("备份已保存并核验");
    await maintenance.getByLabel("已有备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "核验备份", exact: true }).click();
    await maintenance.getByLabel("新恢复目录", { exact: true }).fill(restored);
    await maintenance
      .getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权", exact: true })
      .check();
    await maintenance.getByRole("button", { name: "恢复到新目录", exact: true }).click();
    await expect(maintenance).toContainText("已恢复到独立目录并核验，未启动服务");
    const manifest = (await rpc(page.request, "system.backup.inspect", { backup: restored })).backup
      .manifest;
    expect(manifest.owners).toHaveLength(2);
    expect(manifest.dependencies.every((item: { included: boolean }) => item.included)).toBe(true);
    expect(
      manifest.dependencies.some(
        (item: { object: { kind: string; id: string } }) =>
          item.object.kind === "model_plan" && item.object.id === plan.object.id,
      ),
    ).toBe(true);
    expect(
      manifest.dependencies.some(
        (item: { object: { kind: string; id: string } }) =>
          item.object.kind === "factor_result" && item.object.id === "model-ui-source",
      ),
    ).toBe(true);
    expect(
      manifest.files.some(
        (file: { path: string; format: string }) =>
          file.path === "tasks.sqlite" && file.format === "sqlite",
      ),
    ).toBe(true);
    expect(
      manifest.dependencies.some(
        (item: { object: { kind: string; id: string; version: string } }) =>
          item.object.kind === "model_training_result" &&
          item.object.id === "model-ui-training" &&
          item.object.version === original.task.result_digest,
      ),
    ).toBe(true);
    expect(
      manifest.dependencies.some(
        (item: { object: { kind: string; id: string; version: string } }) =>
          item.object.kind === "model_time_plan" &&
          item.object.id === timePlan.object.id &&
          item.object.version === timePlan.object.version,
      ),
    ).toBe(true);
    expect(
      manifest.files.some(
        (file: { path: string; sha256: string }) =>
          file.path === `model-time-ui/results/${timeOriginal.task.attempt}.pb` &&
          file.sha256 === timeOriginal.task.result_digest,
      ),
    ).toBe(true);
    expect(await stopped()).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
