import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
test("fixed CTA candidate saves a non-running draft and requires explicit backtest admission with original evidence", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  await seedDataset(
    page.request,
    [100, 102, 106, 110, 104, 98, 94, 100, 107, 112, 103, 95],
    "strategy-source-fixture",
  );
  await rpc(page.request, "backtest.submit", {
    id: "strategy-reference",
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
            source: "isolated explicit fixture costs",
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
  const tasks = async () => (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  await expect
    .poll(
      async () =>
        (await tasks()).find((task: { id: string }) => task.id === "strategy-reference")?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const reference = (
    await taskRpc(page.request, "research", "task.result.read", { id: "strategy-reference" })
  ).task_result;
  await openView(page, "strategies");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByLabel("来源回测任务", { exact: true }).fill("strategy-reference");
  await workspace.getByLabel("候选名称", { exact: true }).fill("合成CTA候选");
  await workspace.getByLabel("候选用途", { exact: true }).fill("固定原件研究交接");
  await workspace
    .getByLabel("候选限制", { exact: true })
    .fill("仅合成测试，无预测有效性或实盘许可");
  await expect(workspace.getByRole("button", { name: "冻结策略候选", exact: true })).toBeDisabled();
  await workspace.getByRole("button", { name: "读取固定回测假设", exact: true }).click();
  await expect(workspace.getByRole("region", { name: "固定交接假设", exact: true })).toBeVisible();
  await workspace.getByRole("button", { name: "冻结策略候选", exact: true }).click();
  const definition = workspace.getByRole("region", { name: "固定候选定义", exact: true });
  await expect(
    definition.getByText("原回测仅作参考，不是此包的已使用记录。", { exact: true }),
  ).toBeVisible();
  const first = (await rpc(page.request, "research.strategy.list", { after: "", limit: 30 }))
    .research_strategy.packages;
  expect(first).toHaveLength(1);
  const candidate = first[0];
  expect(candidate.reference_result.version).toBe(reference.task.result_digest);
  expect(candidate.definition.program_artifact).toBe(reference.task.program_artifact);
  await workspace.getByRole("button", { name: "保存交付草稿，不运行", exact: true }).click();
  await expect(workspace.getByText("草稿已保存，尚无正式接纳", { exact: true })).toBeVisible();
  const drafts = (
    await rpc(page.request, "research.strategy.delivery.list", {
      package_id: candidate.object.id,
      after: "",
      limit: 30,
    })
  ).research_strategy.deliveries;
  expect(drafts).toHaveLength(1);
  const draft = drafts[0];
  expect((await tasks()).map((value: { id: string }) => value.id)).toEqual(["strategy-reference"]);
  await expect(
    workspace.getByRole("button", { name: "显式提交固定回测", exact: true }),
  ).toBeDisabled();
  await workspace.getByRole("button", { name: "读取固定回测假设", exact: true }).click();
  await workspace
    .getByRole("checkbox", { name: "确认原固定回测假设，显式提交研究任务", exact: true })
    .check();
  await workspace.getByRole("button", { name: "显式提交固定回测", exact: true }).click();
  await expect(
    workspace.getByText("Research已接纳；不代表任务成功", { exact: true }),
  ).toBeVisible();
  await expect
    .poll(
      async () =>
        (await tasks()).find((value: { id: string }) => value.id === draft.target_task_id)?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const result = (
    await taskRpc(page.request, "research", "task.result.read", { id: draft.target_task_id })
  ).task_result;
  expect(result.task.strategy_use).toEqual({ package: candidate.object, delivery: draft.object });
  expect(result.input_sha256).toBe(reference.input_sha256);
  expect(result.experiment).toEqual(reference.experiment);
  expect(result.result).toEqual(reference.result);
  await workspace.getByRole("button", { name: "查看交付回测正式结果", exact: true }).click();
  await workspace.getByText("实验参数", { exact: true }).click();
  await workspace.getByText("数据与程序身份", { exact: true }).first().click();
  await expect(workspace.getByText("接纳时固定策略候选", { exact: true })).toBeVisible();
  await expect(
    workspace.getByText(
      `${candidate.object.node}/${candidate.object.service}/${candidate.object.id} / ${candidate.object.version}`,
      { exact: true },
    ),
  ).toBeVisible();
  await expect(
    workspace.getByText(
      `${draft.object.node}/${draft.object.service}/${draft.object.id} / ${draft.object.version}`,
      { exact: true },
    ),
  ).toBeVisible();
  await openView(page, "strategies");
  await workspace.getByLabel("弃用原因", { exact: true }).fill("退出新交接，保留原证据");
  await workspace.getByRole("button", { name: "弃用候选，保留既有任务", exact: true }).click();
  await expect(
    definition.getByText("已弃用 · 退出新交接，保留原证据", { exact: true }),
  ).toBeVisible();
  await workspace.getByRole("button", { name: "查询原草稿与接纳状态", exact: true }).click();
  await expect(
    workspace.getByText("Research已接纳；不代表任务成功", { exact: true }),
  ).toBeVisible();
  const state = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    service: state.data.service,
    action: "stop",
  });
  await rpc(page.request, "node.action", {
    service: state.research_service.service,
    action: "restart",
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.find(
            (value: { id: string }) => value.id === state.research_service.service,
          ).health,
      { timeout: 20000 },
    )
    .toBe("ready");
  expect(
    (await rpc(page.request, "research.strategy.delivery.read", { id: draft.object.id }))
      .research_strategy.target_accepted,
  ).toBe(true);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: draft.target_task_id }))
      .task_result,
  ).toEqual(result);
  expect(
    (await rpc(page.request, "research.strategy.read", { id: candidate.object.id }))
      .research_strategy.package.retirement_reason,
  ).toBe("退出新交接，保留原证据");
  await page.goto("/?screen=settings&category=resources");
  const catalogue = page.getByRole("region", { name: "业务资产目录", exact: true });
  await catalogue
    .getByRole("combobox", { name: "资产所属域", exact: true })
    .selectOption("research");
  await catalogue
    .getByRole("combobox", { name: "资产类别", exact: true })
    .selectOption("strategy_package");
  await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
  await expect(catalogue.getByText(candidate.name, { exact: true })).toBeVisible();
  await expect(catalogue.getByText("已退役", { exact: true })).toBeVisible();
  await catalogue
    .getByRole("combobox", { name: "资产类别", exact: true })
    .selectOption("strategy_delivery");
  await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
  await expect(catalogue.getByText(draft.target_task_id, { exact: true })).toBeVisible();
  await expect(catalogue.getByText("已固定（只读）", { exact: true })).toBeVisible();
  await expect(catalogue.getByRole("button", { name: "查看引用与维护", exact: true })).toHaveCount(
    0,
  );
  await rpc(page.request, "node.action", {
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
  const temporary = mkdtempSync(join(tmpdir(), "asterion-strategy-cold-browser-"));
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
    const verified = (await rpc(page.request, "system.backup.inspect", { backup: restored })).backup
      .manifest;
    expect(verified.owners).toHaveLength(2);
    expect(verified.dependencies.every((item: { included: boolean }) => item.included)).toBe(true);
    expect(
      verified.dependencies.some(
        (item: { object: { kind: string; id: string } }) =>
          item.object.kind === "strategy_package" && item.object.id === candidate.object.id,
      ),
    ).toBe(true);
    expect(
      verified.dependencies.some(
        (item: { object: { kind: string; id: string } }) =>
          item.object.kind === "strategy_delivery" && item.object.id === draft.object.id,
      ),
    ).toBe(true);
    expect(await stopped()).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
