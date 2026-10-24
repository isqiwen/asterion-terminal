import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
test("fixed market model replay requires explicit policies and preserves decisions fills and cold dependencies", async ({
  page,
}) => {
  test.setTimeout(180000);
  page.setDefaultTimeout(15000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  await seedDataset(
    page.request,
    Array.from({ length: 720 }, (_, i) => 100 + Math.floor(i / 3) + (i % 3)),
    "model-account-ui-data",
    { minuteDays: ["2026-09-01", "2026-09-02", "2026-09-03"] },
  );
  const wait = async (id: string) => {
    await expect
      .poll(
        async () =>
          (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
            (value: { id: string }) => value.id === id,
          )?.state,
        { timeout: 30000 },
      )
      .toBe("succeeded");
    return (await taskRpc(page.request, "research", "task.result.read", { id })).task_result;
  };
  await rpc(page.request, "factor.submit", {
    id: "replay-ui-factor",
    series: { kind: "bars" },
    factor: "momentum",
    lookbacks: [2],
    horizon: 2,
    evaluation: { mode: "holdout", split_index: 600 },
    retain_observations: true,
  });
  const source = await wait("replay-ui-factor");
  const state = await rpc(page.request, "runtime.snapshot");
  const modelDraft = (
    await rpc(page.request, "research.model.save", {
      id: "save-replay-ui-model",
      draft_id: "replay-ui-model",
      expected_digest: "",
      specification: {
        name: "行情模拟原模型",
        purpose: "隔离行情因果账户验收",
        limitations: "历史可得性未知，不授权实盘",
        source: {
          node: state.research_service.owner_node_id,
          service: state.research_service.service,
          domain: "research",
          kind: "factor_result",
          id: source.id,
          version: source.task.result_digest,
        },
        method: "linear",
        fit_intercept: true,
        alpha: null,
        training_begin: 0,
        training_end: 240,
        standardize: true,
        weights: "equal",
        mode: "post_hoc_diagnostic",
        run_budget: 1,
        time_budget_seconds: 30,
        valid_until_ms: 4102444800000,
      },
    })
  ).research_model.draft;
  const plan = (
    await rpc(page.request, "research.model.fix", {
      id: "fix-replay-ui-model",
      draft_id: modelDraft.object.id,
      expected_digest: modelDraft.object.version,
      plan_id: "replay-ui-plan",
    })
  ).research_model.plan;
  const timePlan = (
    await rpc(page.request, "research.model.time.save", {
      id: "save-replay-ui-time",
      plan_id: "replay-ui-time-plan",
      specification: {
        name: "行情模拟原折",
        purpose: "仅首日训练次日决策",
        limitations: "最终留出不执行",
        recipe: plan.object,
        folds: [
          { training_begin: 0, training_end: 240, validation_begin: 240, validation_end: 480 },
        ],
        run_budget: 1,
        time_budget_seconds: 30,
        valid_until_ms: 4102444700000,
      },
    })
  ).research_model.time_plan;
  await rpc(page.request, "research.model.time.train", {
    id: "replay-ui-time",
    plan: timePlan.object,
  });
  const originalTime = await wait("replay-ui-time");
  const mapping = (
    await rpc(page.request, "research.model.mapping.save", {
      id: "save-replay-ui-mapping",
      mapping_id: "replay-ui-directions",
      specification: {
        name: "行情模拟固定映射",
        purpose: "明确条件研究，无交易授权",
        model: {
          ...plan.object,
          kind: "model_time_validation_result",
          id: originalTime.id,
          version: originalTime.task.result_digest,
        },
        short_threshold: "-0.000001",
        long_threshold: "0.000001",
        middle: "no_new_direction",
        valid_until_ms: 4102444600000,
      },
    })
  ).research_model.mapping;
  await page.reload();
  await openView(page, "backtest");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "100000"],
    ["每手保证金", "100"],
    ["滑点（跳）", "1"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  const priorIds = new Set(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
      (row: { id: string }) => row.id,
    ),
  );
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  await expect(
    workspace.getByText("任务已提交，可在任务中心查看进度。", { exact: true }),
  ).toBeVisible();
  const base = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
    (row: { id: string; kind: string }) => row.kind === "backtest" && !priorIds.has(row.id),
  );
  expect(base).toBeTruthy();
  const reference = await wait(base.id);
  await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
  await workspace
    .getByRole("row")
    .filter({ hasText: base.id })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  const setup = workspace.getByRole("region", { name: "固定行情模型模拟", exact: true });
  await setup.getByRole("button", { name: "读取固定逐折映射", exact: true }).click();
  await setup.getByLabel("固定逐折映射", { exact: true }).selectOption(mapping.object.id);
  await setup.getByLabel("模拟原折", { exact: true }).selectOption({ value: "0" });
  await setup.getByLabel("目标手数", { exact: true }).fill("1");
  await setup.getByLabel("允许方向", { exact: true }).selectOption("both");
  await setup.getByLabel("模型处理延迟（bar）", { exact: true }).fill("1");
  await setup.getByLabel("模拟时间预算（秒）", { exact: true }).fill("30");
  await expect(setup.getByRole("button", { name: "创建模型模拟任务", exact: true })).toBeDisabled();
  await setup
    .getByRole("checkbox", {
      name: "我明确假设原完整bar在其收盘可用；此模拟不是历史可交易或实盘授权证据。",
      exact: true,
    })
    .check();
  await setup.getByRole("button", { name: "创建模型模拟任务", exact: true }).click();
  const accepted = setup.getByRole("status");
  await expect(accepted).toContainText("模型模拟任务已接纳");
  const id = await accepted.locator("code").innerText();
  const result = await wait(id);
  expect(result.experiment.paper).toEqual(reference.experiment.paper);
  expect(result.experiment.model_replay.mapping).toEqual(mapping);
  expect(result.result.account.cursor).toBe(480);
  expect(result.result.model_replay.fit.started_index).toBe(240);
  expect(result.result.model_replay.fit.ready_index).toBe(241);
  expect(result.result.model_replay.fills.length).toBeGreaterThan(0);
  for (const fill of result.result.model_replay.fills)
    expect(fill.source_index).toBe(fill.decision_source_index + 1);
  await page.reload();
  await openView(page, "backtest");
  await workspace
    .getByRole("row")
    .filter({ hasText: id })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  const evidence = workspace.getByRole("region", { name: "模型行情模拟证据", exact: true });
  await expect(evidence).toContainText("历史可得时间未知");
  await expect(evidence).toContainText(result.result.model_replay.fit.ready_boundary_ns);
  for (let i = 0; i < 4; ++i)
    await evidence.getByRole("button", { name: "下一页", exact: true }).click();
  const decision = evidence
    .getByRole("table", { name: "模型逐bar决策原件", exact: true })
    .getByRole("row")
    .filter({ has: page.getByRole("cell", { name: "241", exact: true }) });
  await expect(decision.getByRole("cell").nth(4)).toHaveText(
    String(result.result.model_replay.decisions[241].prediction),
  );
  await expect(
    evidence.getByRole("table", { name: "模型决策与真实模拟成交关联", exact: true }),
  ).toContainText(result.result.model_replay.fills[0].fill_id);
  await openView(page, "strategies");
  await workspace.getByLabel("来源回测任务", { exact: true }).fill(id);
  await workspace.getByLabel("候选名称", { exact: true }).fill("固定模型模拟候选");
  await workspace.getByLabel("候选用途", { exact: true }).fill("原模型政策的明确回测交接");
  await workspace.getByLabel("候选限制", { exact: true }).fill("历史可得未知；不授权交易");
  await expect(workspace.getByRole("button", { name: "冻结策略候选", exact: true })).toBeDisabled();
  await workspace.getByRole("button", { name: "读取固定回测假设", exact: true }).click();
  await workspace.getByRole("button", { name: "冻结策略候选", exact: true }).click();
  const packagePanel = workspace.getByRole("region", { name: "固定候选定义", exact: true });
  await expect(packagePanel).toContainText("固定模型决策");
  await expect(packagePanel).toContainText(mapping.object.version);
  await expect(packagePanel).toContainText(timePlan.object.version);
  const candidate = (
    await rpc(page.request, "research.strategy.list", { after: "", limit: 30 })
  ).research_strategy.packages.find((item: { name: string }) => item.name === "固定模型模拟候选");
  expect(candidate.definition.model_replay).toEqual(result.experiment.model_replay);
  expect(candidate.definition.strategy).toBeUndefined();
  expect(candidate.evidence_relation).toBe("reference_only");
  const beforeDraft = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
    (item: { id: string }) => item.id,
  );
  await workspace.getByRole("button", { name: "保存交付草稿，不运行", exact: true }).click();
  await expect(workspace.getByText("草稿已保存，尚无正式接纳", { exact: true })).toBeVisible();
  const draft = (
    await rpc(page.request, "research.strategy.delivery.list", {
      package_id: candidate.object.id,
      after: "",
      limit: 30,
    })
  ).research_strategy.deliveries[0];
  expect(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
      (item: { id: string }) => item.id,
    ),
  ).toEqual(beforeDraft);
  await expect(
    workspace.getByRole("button", { name: "显式提交固定回测", exact: true }),
  ).toBeDisabled();
  await workspace.getByRole("button", { name: "读取固定回测假设", exact: true }).click();
  await workspace
    .getByRole("checkbox", { name: "确认原固定回测假设，显式提交研究任务", exact: true })
    .check();
  await workspace.getByRole("button", { name: "显式提交固定回测", exact: true }).click();
  const delivered = await wait(draft.target_task_id);
  expect(delivered.experiment).toEqual(result.experiment);
  expect(delivered.result.account).toEqual(result.result.account);
  expect(delivered.result.model_replay.decisions).toEqual(result.result.model_replay.decisions);
  expect(delivered.task.strategy_use.package).toEqual(candidate.object);
  expect(delivered.task.strategy_use.delivery).toEqual(draft.object);
  expect(result.task.strategy_use).toBeUndefined();
  await workspace.getByRole("button", { name: "查看交付回测正式结果", exact: true }).click();
  await expect(
    workspace.getByRole("region", { name: "模型行情模拟证据", exact: true }),
  ).toBeVisible();
  await workspace.getByText("实验参数", { exact: true }).click();
  await expect(workspace.getByText("接纳时固定策略候选", { exact: true })).toBeVisible();
  await openView(page, "strategies");
  await workspace.getByLabel("弃用原因", { exact: true }).fill("结束新增交付，保留正式模型证据");
  await workspace.getByRole("button", { name: "弃用候选，保留既有任务", exact: true }).click();
  await expect(packagePanel).toContainText("已弃用");
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
  expect((await taskRpc(page.request, "research", "task.result.read", { id })).task_result).toEqual(
    result,
  );
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: draft.target_task_id }))
      .task_result,
  ).toEqual(delivered);
  expect(
    (await rpc(page.request, "research.strategy.delivery.read", { id: draft.object.id }))
      .research_strategy.target_accepted,
  ).toBe(true);
  const services = [state.data.service, state.research_service.service];
  for (const service of services)
    await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.filter((row: { id: string }) => services.includes(row.id))
          .every(
            (row: { desired_running: boolean; pid: number }) => !row.desired_running && !row.pid,
          ),
      { timeout: 15000 },
    )
    .toBe(true);
  const temporary = mkdtempSync(join(tmpdir(), "asterion-model-replay-cold-"));
  try {
    const backup = join(temporary, "backup"),
      restored = join(temporary, "restored");
    await page.goto("/?screen=settings&category=backup");
    const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    for (const service of services)
      await maintenance.getByRole("checkbox", { name: new RegExp(service) }).check();
    await maintenance.getByLabel("新备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "预览备份", exact: true }).click();
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
    expect(manifest.dependencies.every((row: { included: boolean }) => row.included)).toBe(true);
    expect(
      manifest.dependencies.some(
        (row: { object: { kind: string; id: string; version: string } }) =>
          row.object.kind === "strategy_package" &&
          row.object.id === candidate.object.id &&
          row.object.version === candidate.object.version,
      ),
    ).toBe(true);
    expect(
      manifest.dependencies.some(
        (row: { object: { kind: string; id: string; version: string } }) =>
          row.object.kind === "model_mapping" &&
          row.object.id === mapping.object.id &&
          row.object.version === mapping.object.version,
      ),
    ).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
