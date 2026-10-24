import { expect } from "./test";
import type { Page } from "@playwright/test";
import { rpc, taskRpc, seedHistory } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { explicitPortfolioDelivery } from "./portfolio-explicit-flow";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

export async function portfolioTargetsFlow(page: Page, partial: boolean) {
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const days = ["2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-28"];
  const first = await seedHistory(
    page.request,
    days.map(() => 1000),
    "portfolio-rb",
    { tablesOnly: true, dailyPrices: true, dailyDays: days, dailyVolume: partial ? 10 : 10000 },
  );
  const second = await seedHistory(
    page.request,
    days.map(() => 1000),
    "portfolio-hc",
    {
      product: "hc",
      keep: true,
      tablesOnly: true,
      dailyPrices: true,
      dailyDays: days,
      dailyVolume: partial ? 10 : 10000,
    },
  );
  for (const [product, source] of [
    ["rb", first],
    ["hc", second],
  ] as const)
    await rpc(page.request, "data.dataset.daily", {
      contract_id: `SHFE/${product}/2026-10`,
      daily: source.daily,
      begin_day: days[0],
      end_day: days.at(-1)!,
    });
  await page.goto("/");
  await openView(page, "backtest");
  await page.getByRole("button", { name: "新建回测", exact: true }).click();
  await expect(page.getByRole("list", { name: "已选合约" })).toContainText("rb2610");
  await expect(page.getByRole("list", { name: "已选合约" })).toContainText("hc2610");
  await page.getByRole("button", { name: "下一步", exact: true }).click();
  await page.getByLabel("使用固定权重组合政策（仅完整普通日线）", { exact: true }).check();
  for (const [label, value] of [
    ["原合约名义权重 SHFE/rb2610", "0.4"],
    ["原合约名义权重 SHFE/hc2610", "-0.2"],
    ["组合总绝对名义权重预算", "0.8"],
    ["组合净名义权重预算", "0.3"],
    ["每隔共同完整bar调仓（1–10000，必填）", "2"],
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
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
    ])
      await costs.getByLabel(label, { exact: true }).fill(value);
  }
  await page.getByRole("button", { name: "下一步", exact: true }).click();
  const confirmation = page.getByText("固定组合名义政策", { exact: true }).locator("..");
  await expect(confirmation).toContainText("SHFE/rb2610：0.4");
  await expect(confirmation).toContainText("SHFE/hc2610：-0.2");
  expect((await rpc(page.request, "runtime.snapshot")).research_service.tasks).toHaveLength(0);
  await page.getByRole("button", { name: "开始回测", exact: true }).click();
  const progress = page.getByRole("region", { name: "回测进度", exact: true });
  await progress.getByRole("button", { name: "查看结果", exact: true }).click({ timeout: 30000 });
  const tasks = (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  expect(tasks).toHaveLength(1);
  const id = tasks[0].id;
  const evidence = (await taskRpc(page.request, "research", "task.result.read", { id }))
    .task_result;
  expect(evidence.experiment.strategies).toHaveLength(1);
  expect(evidence.experiment.strategies[0].rule.kind).toBe("fixed_weights");
  expect(evidence.result.portfolio_targets).toHaveLength(3);
  expect(evidence.result.portfolio_targets[0]).toMatchObject({
    equity: "100000",
    trading_day: days[1],
    has_next_bar: true,
    members: [
      { symbol: "rb2610", quantity: "4", intent_id: "strategy.2", source_index: 2 },
      { symbol: "hc2610", quantity: "-2", intent_id: "strategy.3", source_index: 3 },
    ],
  });
  expect(evidence.result.account.fees).toBe(partial ? "6" : "20");
  expect(evidence.result.account.balance).toBe(partial ? "99994" : "99980");
  expect(evidence.result.account.orders).toHaveLength(partial ? 3 : 4);
  expect(evidence.result.portfolio_orders).toHaveLength(partial ? 3 : 4);
  expect(evidence.result.portfolio_fills).toHaveLength(partial ? 3 : 4);
  expect(evidence.result.portfolio_fills[0]).toMatchObject({
    decision_source_index: 2,
    source_index: 4,
  });
  const targets = page.getByRole("region", { name: "固定组合目标与实际执行", exact: true });
  await expect(
    targets.getByRole("table", { name: "全部原组合调仓边界" }).getByRole("row"),
  ).toHaveCount(4);
  const fills = targets.getByRole("table", { name: "原组合意图的实际成交", exact: true });
  await expect(fills).toContainText(evidence.result.portfolio_fills[0].fill_id);
  const firstDecision = evidence.result.portfolio_targets[0];
  expect(
    firstDecision.members.every(
      (m: { observation: { positions: unknown[]; working_orders: unknown[] } }) =>
        m.observation.positions.length === 0 && m.observation.working_orders.length === 0,
    ),
  ).toBe(true);
  await targets.getByRole("button", { name: "查看原组合目标与成交", exact: true }).nth(1).click();
  const originalHoldings = targets.getByRole("table", {
    name: "原调仓边界实际持仓批次",
    exact: true,
  });
  const heldRows = originalHoldings.getByRole("row");
  await expect(heldRows).toHaveCount(3);
  await expect(heldRows.nth(1).getByRole("cell").nth(3)).toHaveText(partial ? "1" : "4");
  await expect(heldRows.nth(2).getByRole("cell").nth(3)).toHaveText(partial ? "1" : "2");
  await expect(originalHoldings).toContainText("昨仓");
  if (partial) {
    const order = evidence.result.account.orders[2];
    expect(order).toMatchObject({
      symbol: "rb2610",
      quantity: "2",
      filled: "1",
      state: "cancelled",
    });
    expect(evidence.result.fill_days.map((f: { trading_day: string }) => f.trading_day)).toEqual([
      days[2],
      days[2],
      days[4],
    ]);
    const accepted = targets.getByRole("table", { name: "原组合意图的实际接纳委托", exact: true });
    await expect(accepted).toContainText(order.id);
    await expect(accepted).toContainText("已撤余量");
    const cells = accepted.getByRole("row").nth(1).getByRole("cell");
    await expect(cells.nth(7)).toHaveText("2");
    await expect(cells.nth(8)).toHaveText("1");
  }
  await targets.getByRole("button", { name: "查看原组合目标与成交", exact: true }).last().click();
  await expect(targets).toContainText("本轮未记录实际成交，不将目标或委托当作成交。");
  const downloading = page.waitForEvent("download");
  await page.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const download = await downloading;
  const path = await download.path();
  expect(path).toBeTruthy();
  const document = JSON.parse(readFileSync(path!, "utf8"));
  expect(document.format).toBe("asterion.research.evidence");
  expect(document.source.task_id).toBe(id);
  expect(document.source.result_sha256).toBe(evidence.task.result_digest);
  expect(document.source.program_artifact).toBe(evidence.task.program_artifact);
  expect(document.result).toEqual(evidence.result);
  expect(document.experiment).toEqual(evidence.experiment);
  await openView(page, "strategies");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  for (const [label, value] of [
    ["来源回测任务", id],
    ["候选名称", "明确组合研究候选"],
    ["候选用途", "原固定条件组合交付"],
    ["候选限制", "合成事后参考，不是独立验证或实盘权限"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "读取固定回测假设", exact: true }).click();
  await workspace.getByRole("button", { name: "冻结策略候选", exact: true }).click();
  const definition = workspace.getByRole("region", { name: "固定候选定义", exact: true });
  await expect(definition).toContainText("SHFE/rb2610 · SHFE/hc2610");
  await expect(definition).toContainText("0.4");
  await expect(definition).toContainText("-0.2");
  const candidates = (await rpc(page.request, "research.strategy.list", { after: "", limit: 30 }))
    .research_strategy.packages;
  expect(candidates).toHaveLength(1);
  const candidate = candidates[0];
  expect(candidate.definition.contract).toBeUndefined();
  expect(candidate.definition.portfolio.contracts).toHaveLength(2);
  expect(candidate.definition.portfolio.strategy).toEqual(evidence.experiment.strategies[0]);
  expect((await rpc(page.request, "runtime.snapshot")).research_service.tasks).toHaveLength(1);
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
  expect((await rpc(page.request, "runtime.snapshot")).research_service.tasks).toHaveLength(1);
  await workspace.getByRole("button", { name: "读取固定回测假设", exact: true }).click();
  await workspace
    .getByRole("checkbox", { name: "确认原固定回测假设，显式提交研究任务", exact: true })
    .check();
  await workspace.getByRole("button", { name: "显式提交固定回测", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string }) => t.id === draft.target_task_id,
        )?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const used = (
    await taskRpc(page.request, "research", "task.result.read", { id: draft.target_task_id })
  ).task_result;
  expect(used.task.strategy_use).toEqual({ package: candidate.object, delivery: draft.object });
  expect(used.input_sha256).toBe(evidence.input_sha256);
  expect(used.result).toEqual(evidence.result);
  await workspace.getByRole("button", { name: "查看交付回测正式结果", exact: true }).click();
  await expect(
    page.getByRole("region", { name: "固定组合目标与实际执行", exact: true }),
  ).toBeVisible();
  const usedDownloadPromise = page.waitForEvent("download");
  await page.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const usedDownload = await usedDownloadPromise;
  const usedPath = await usedDownload.path();
  expect(usedPath).toBeTruthy();
  const usedDocument = JSON.parse(readFileSync(usedPath!, "utf8"));
  expect(usedDocument.source.task_id).toBe(draft.target_task_id);
  expect(usedDocument.source.program_artifact).toBe(used.task.program_artifact);
  expect(usedDocument.strategy_use).toEqual(used.task.strategy_use);
  expect(usedDocument.result).toEqual(used.result);
  const configured = partial
    ? null
    : await explicitPortfolioDelivery(page, candidate, evidence, days);
  const context = (await rpc(page.request, "runtime.snapshot")).research_service;
  for (const action of ["stop", "start"])
    await rpc(page.request, "node.action", { id: "local", service: context.service, action });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).research_service?.online, {
      timeout: 10000,
    })
    .toBe(true);
  expect((await taskRpc(page.request, "research", "task.result.read", { id })).task_result).toEqual(
    evidence,
  );
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: draft.target_task_id }))
      .task_result,
  ).toEqual(used);
  expect(
    (await rpc(page.request, "research.strategy.read", { id: candidate.object.id }))
      .research_strategy.package,
  ).toEqual(candidate);
  expect(
    (await rpc(page.request, "research.strategy.delivery.read", { id: draft.object.id }))
      .research_strategy.target_accepted,
  ).toBe(true);
  if (configured) {
    expect(
      (await taskRpc(page.request, "research", "task.result.read", { id: configured.id }))
        .task_result,
    ).toEqual(configured.result);
    expect(
      (await rpc(page.request, "research.strategy.delivery.read", { id: configured.delivery_id }))
        .research_strategy.target_accepted,
    ).toBe(true);
  }
  const temporary = mkdtempSync(join(tmpdir(), "asterion-portfolio-evidence-"));
  try {
    const state = await rpc(page.request, "runtime.snapshot");
    const selected = [state.data.service, state.research_service.service];
    for (const service of selected)
      await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
    await expect
      .poll(
        async () =>
          (await rpc(page.request, "runtime.snapshot")).nodes
            .find((n: { id: string }) => n.id === "local")
            .health.services.filter((s: { id: string }) => selected.includes(s.id))
            .every((s: { desired_running: boolean; pid: number }) => !s.desired_running && !s.pid),
        { timeout: 10000 },
      )
      .toBe(true);
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
    const manifest = (await rpc(page.request, "system.backup.inspect", { backup })).backup.manifest;
    const owner = manifest.owners.findIndex(
      (o: { domain: string; service: string }) =>
        o.domain === "research" && o.service === context.service,
    );
    expect(owner).toBeGreaterThanOrEqual(0);
    const originalBytes = readFileSync(
      join(backup, "owners", String(owner), id, "results", "1.pb"),
    );
    const originalInput = readFileSync(join(backup, "owners", String(owner), id, "input.pb"));
    const usedBytes = readFileSync(
      join(backup, "owners", String(owner), draft.target_task_id, "results", "1.pb"),
    );
    const usedInput = readFileSync(
      join(backup, "owners", String(owner), draft.target_task_id, "input.pb"),
    );
    const configuredBytes = configured
      ? {
          input: readFileSync(join(backup, "owners", String(owner), configured.id, "input.pb")),
          result: readFileSync(
            join(backup, "owners", String(owner), configured.id, "results", "1.pb"),
          ),
        }
      : null;
    await maintenance.getByLabel("已有备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "核验备份", exact: true }).click();
    await maintenance.getByLabel("新恢复目录", { exact: true }).fill(restored);
    await maintenance
      .getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权", exact: true })
      .check();
    await maintenance.getByRole("button", { name: "恢复到新目录", exact: true }).click();
    await expect(maintenance).toContainText("已恢复到独立目录并核验，未启动服务");
    const recovered = (await rpc(page.request, "system.backup.inspect", { backup: restored }))
      .backup.manifest;
    expect(recovered.owners).toHaveLength(2);
    expect(recovered.dependencies.every((d: { included: boolean }) => d.included)).toBe(true);
    expect(readFileSync(join(restored, "owners", String(owner), id, "results", "1.pb"))).toEqual(
      originalBytes,
    );
    expect(readFileSync(join(restored, "owners", String(owner), id, "input.pb"))).toEqual(
      originalInput,
    );
    expect(
      readFileSync(
        join(restored, "owners", String(owner), draft.target_task_id, "results", "1.pb"),
      ),
    ).toEqual(usedBytes);
    expect(
      readFileSync(join(restored, "owners", String(owner), draft.target_task_id, "input.pb")),
    ).toEqual(usedInput);
    if (configured && configuredBytes) {
      expect(
        readFileSync(join(restored, "owners", String(owner), configured.id, "input.pb")),
      ).toEqual(configuredBytes.input);
      expect(
        readFileSync(join(restored, "owners", String(owner), configured.id, "results", "1.pb")),
      ).toEqual(configuredBytes.result);
    }
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
}
