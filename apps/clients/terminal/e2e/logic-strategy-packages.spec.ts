import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
test("checked logic assembles a package and a non-running draft before explicit real backtest admission", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  // Seeding reopens Data and Research; the page starts after it, so its
  // drafts are keyed to the connection it keeps.
  await seedDataset(
    page.request,
    [100, 102, 106, 110, 104, 98, 94, 100, 107, 112, 103, 95],
    "logic-package-source",
  );
  await page.goto("/");
  await openView(page, "logic");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByLabel("名称", { exact: true }).fill("固定双均线");
  await workspace.getByLabel("研究目的", { exact: true }).fill("合成因果逻辑，不证明收益");
  await workspace.getByRole("button", { name: "保存不可变规则", exact: true }).click();
  await workspace.getByLabel("样例名称", { exact: true }).fill("事先明确方向");
  await workspace.getByLabel("适用范围", { exact: true }).fill("四个虚构收盘，目标方向核对");
  await workspace.getByRole("button", { name: "保存事先预期，不运行", exact: true }).click();
  await workspace.getByLabel("试算任务标识", { exact: true }).fill("logic-passed-browser");
  await workspace.getByRole("button", { name: "运行固定样例试算", exact: true }).click();
  const tasks = async () => (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  await expect
    .poll(
      async () =>
        (await tasks()).find((task: { id: string }) => task.id === "logic-passed-browser")?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  await workspace.getByRole("button", { name: "读取原试算结果", exact: true }).click();
  const trial = (
    await taskRpc(page.request, "research", "task.result.read", { id: "logic-passed-browser" })
  ).task_result;
  expect(trial.result.all_match).toBe(true);
  expect(trial.result.decisions).toBe(2);
  await workspace.getByRole("button", { name: "进入策略管理组装固定包", exact: true }).click();
  await expect(workspace.getByLabel("来源试算任务", { exact: true })).toHaveValue(
    "logic-passed-browser",
  );
  await workspace.getByRole("button", { name: "读取已核对试算", exact: true }).click();
  await workspace.getByLabel("候选名称", { exact: true }).fill("由原逻辑组装的候选");
  await workspace.getByLabel("候选用途", { exact: true }).fill("显式研究回测");
  await workspace
    .getByLabel("候选限制", { exact: true })
    .fill("虚构核对不代表真实市场收益或交易许可");
  const state = await rpc(page.request, "runtime.snapshot");
  await workspace
    .getByRole("combobox", { name: "组包合约与周期", exact: true })
    .selectOption(state.datasets[0].revision);
  await workspace.getByLabel("固定目标数量", { exact: true }).fill("1");
  await workspace.getByRole("button", { name: "保存已核对逻辑包，不运行", exact: true }).click();
  await expect(
    workspace.getByRole("region", { name: "固定逻辑包定义", exact: true }),
  ).toBeVisible();
  const packages = (await rpc(page.request, "research.strategy.list", { after: "", limit: 30 }))
    .research_strategy.packages;
  expect(packages).toHaveLength(1);
  const candidate = packages[0];
  expect(candidate.evidence_relation).toBe("logic_trial");
  expect(candidate.reference_result).toBeNull();
  expect(candidate.logic.result.id).toBe(trial.id);
  expect(candidate.logic.result.version).toBe(trial.task.result_digest);
  expect(candidate.logic.definition.object).toEqual(trial.experiment.definition.object);
  await workspace.getByLabel("目标回测任务", { exact: true }).fill("logic-backtest-browser");
  await workspace.getByRole("button", { name: "保存交付草稿，不运行", exact: true }).click();
  await expect(workspace.getByText("草稿已保存，尚无正式接纳", { exact: true })).toBeVisible();
  expect((await tasks()).filter((task: { kind: string }) => task.kind === "backtest")).toHaveLength(
    0,
  );
  const drafts = (
    await rpc(page.request, "research.strategy.delivery.list", {
      package_id: candidate.object.id,
      after: "",
      limit: 30,
    })
  ).research_strategy.deliveries;
  expect(drafts).toHaveLength(1);
  const draft = drafts[0];
  expect(draft.reference_result).toBeNull();
  expect(draft.logic_trial).toEqual(candidate.logic.result);
  await workspace.getByRole("button", { name: "配置交付回测草稿", exact: true }).click();
  await expect(
    workspace.getByRole("region", { name: "固定逻辑交付回测", exact: true }),
  ).toContainText("logic-backtest-browser");
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(workspace.getByLabel("快均线", { exact: true })).toHaveValue("1");
  await expect(workspace.getByLabel("慢均线", { exact: true })).toHaveValue("3");
  await expect(workspace.getByLabel("快均线", { exact: true })).toBeDisabled();
  await expect(workspace.getByLabel("目标手数", { exact: true })).toBeDisabled();
  for (const [label, value] of [
    ["初始资金", "10000"],
    ["每手保证金", "100"],
    ["滑点（跳）", "0"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "1"],
    ["总持仓量上限", "1"],
    ["在途委托数上限", "4"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await tasks()).find((task: { id: string }) => task.id === "logic-backtest-browser")?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const actual = (
    await taskRpc(page.request, "research", "task.result.read", { id: "logic-backtest-browser" })
  ).task_result;
  expect(actual.kind).toBe("backtest");
  expect(actual.task.strategy_use.package).toEqual(candidate.object);
  expect(actual.task.strategy_use.delivery).toEqual(draft.object);
  expect(actual.task.strategy_use.request_sha256).toMatch(/^[a-f0-9]{64}$/);
  expect(actual.task.program_artifact).toBe(candidate.definition.program_artifact);
  expect(
    (await rpc(page.request, "research.strategy.delivery.read", { id: draft.object.id }))
      .research_strategy.target_accepted,
  ).toBe(true);
  const progress = workspace.getByRole("region", { name: "回测进度", exact: true });
  await progress.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "回测结果", exact: true });
  await result.getByText("实验参数", { exact: true }).click();
  await result.getByText("数据与程序身份", { exact: true }).first().click();
  await expect(result.getByText("接纳时固定策略候选", { exact: true })).toBeVisible();
  await expect(result).toContainText(candidate.object.id);
  const prior = (
    await taskRpc(page.request, "research", "task.result.read", { id: "logic-passed-browser" })
  ).task_result;
  expect(prior).toEqual(trial);
  const current = await rpc(page.request, "runtime.snapshot");
  const selected = [current.data.service, current.research_service.service];
  for (const service of selected)
    await rpc(page.request, "node.action", { service, action: "stop" });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.filter((item: { id: string }) => selected.includes(item.id))
          .every(
            (item: { desired_running: boolean; pid: number }) => !item.desired_running && !item.pid,
          ),
      { timeout: 10000 },
    )
    .toBe(true);
  const temporary = mkdtempSync(join(tmpdir(), "asterion-logic-package-browser-"));
  try {
    const backup = join(temporary, "backup"),
      restored = join(temporary, "restored");
    await page.goto("/?screen=settings&category=backup");
    const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    for (const service of selected)
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
    expect(manifest.owners).toHaveLength(2);
    expect(manifest.dependencies.every((item: { included: boolean }) => item.included)).toBe(true);
    for (const [kind, id] of [
      ["strategy_package", candidate.object.id],
      ["strategy_delivery", draft.object.id],
      ["logic_trial_result", trial.id],
      ["logic_case", candidate.logic.example.id],
    ])
      expect(
        manifest.dependencies.some(
          (item: { object: { kind: string; id: string } }) =>
            item.object.kind === kind && item.object.id === id,
        ),
      ).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
