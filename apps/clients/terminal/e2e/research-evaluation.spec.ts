import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("fixed performance evaluation preserves the original account through restart and joint restore", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  const days: string[] = [];
  for (let at = Date.UTC(2026, 7, 17); days.length < 30; at += 86400000)
    if (new Date(at).getUTCDay() % 6 !== 0) days.push(new Date(at).toISOString().slice(0, 10));
  await seedDataset(
    page.request,
    days.map((_, i) => 100 + 2 * i - (i % 3 === 2 ? 3 : 0)),
    "evaluation-source",
    { minuteDays: days },
  );
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
    ["滑点（跳）", "0"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  let sourceId = "";
  await expect
    .poll(
      async () => {
        const task = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (row: { kind: string }) => row.kind === "backtest",
        );
        if (task) sourceId = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const source = (await taskRpc(page.request, "research", "task.result.read", { id: sourceId }))
    .task_result;
  await openView(page, "risk");
  const evaluation = page.getByRole("region", { name: "风险分析与资金事实", exact: true });
  await expect(evaluation).toContainText("尾部估计、动态压力和完整策略归因仍未实现");
  await evaluation.getByRole("combobox", { name: "评估来源", exact: true }).selectOption(sourceId);
  await evaluation.getByRole("button", { name: "读取评估来源", exact: true }).click();
  const form = evaluation.getByRole("region", { name: "固定绩效评价", exact: true });
  await expect(form.getByLabel("年期数A（日结算）", { exact: true })).toHaveValue("");
  await expect(form.getByLabel("明确采用零无风险年收益假设", { exact: true })).not.toBeChecked();
  await form
    .getByLabel("评价问题与用途", { exact: true })
    .fill("合成原账户固定方法评价，不代表独立证据或实盘授权");
  await form.getByLabel("年期数A（日结算）", { exact: true }).fill("252");
  await form.getByLabel("明确采用零无风险年收益假设", { exact: true }).check();
  await expect(
    form.getByLabel("保存日采样损失路径（不代表盘中最大风险）", { exact: true }),
  ).not.toBeChecked();
  await form.getByLabel("保存日采样损失路径（不代表盘中最大风险）", { exact: true }).check();
  await expect(
    form.getByLabel("保存本范围末日敞口和资金观察（非盘中峰值）", { exact: true }),
  ).not.toBeChecked();
  await form.getByLabel("保存本范围末日敞口和资金观察（非盘中峰值）", { exact: true }).check();
  await expect(form.getByLabel("保存显式静态价格与保证金压力", { exact: true })).not.toBeChecked();
  await form.getByLabel("保存显式静态价格与保证金压力", { exact: true }).check();
  await form
    .getByLabel("压力用途与范围", { exact: true })
    .fill("固定原单合约持仓的显式资金压力，不代表成交或现行费率");
  await form.getByLabel("情景价格（原价格单位）", { exact: true }).fill("80");
  await form.getByLabel("假设保证金率（比例，0.4表示40%）", { exact: true }).fill("0.4");
  await expect(form.getByRole("button", { name: "计算固定绩效评价", exact: true })).toBeDisabled();
  await form
    .getByLabel("明确采用原纸面资金模型：余额与权益取小，减保证金及原冻结", { exact: true })
    .check();
  await form.getByRole("button", { name: "计算固定绩效评价", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const task = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (row: { kind: string }) => row.kind === "performance_evaluation",
        );
        if (task) id = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const first = (await taskRpc(page.request, "research", "task.result.read", { id })).task_result;
  expect(first.experiment.source.input_sha256).toBe(source.input_sha256);
  expect(first.experiment.source.program_artifact).toBe(source.task.program_artifact);
  expect(first.experiment.request.method).toEqual({
    annual_periods: 252,
    risk_free: "explicit_zero",
  });
  expect(first.result.complete_return_periods).toBe(29);
  expect(first.experiment.request.include_daily_drawdown).toBe(true);
  expect(first.experiment.request.include_exposure).toBe(true);
  expect(first.experiment.request.static_stress).toEqual({
    purpose: "固定原单合约持仓的显式资金压力，不代表成交或现行费率",
    price: "80",
    margin_rate: "0.4",
    funds_model: "paper_balance_equity_minimum",
  });
  expect(first.result.static_stress).toBeTruthy();
  const exposure = first.result.exposure;
  const last = source.result.settlements[source.result.settlements.length - 1];
  expect(exposure.source_index).toBe(source.result.settlements.length - 1);
  expect(exposure.trading_day).toBe(last.trading_day);
  expect(exposure.timestamp_ns).toBe(last.timestamp_ns);
  expect(exposure.balance).toBe(last.balance);
  expect(exposure.funds).toEqual(last.occupancy);
  expect(exposure.positions).toHaveLength(last.contracts.length);
  for (const position of exposure.positions) {
    const original = last.contracts[position.source_contract_index];
    expect(position).toMatchObject({
      venue: original.venue,
      symbol: original.symbol,
      price: original.price,
      net_quantity: original.position_quantity,
      gross_quantity: original.gross_quantity ?? null,
      margin: original.margin ?? null,
    });
  }
  const daily = first.result.daily_drawdown;
  expect(daily.points).toHaveLength(source.result.settlements.length);
  for (const point of daily.points) {
    const original = source.result.settlements[point.source_index];
    expect(point.trading_day).toBe(original.trading_day);
    expect(point.timestamp_ns).toBe(original.timestamp_ns === "0" ? null : original.timestamp_ns);
    expect(point.equity).toBe(original.equity);
  }
  expect(first.result.limitations).toContain("RISK_AND_STRESS_INCOMPLETE");
  expect(first.result.sharpe.state).toBe("AVAILABLE");
  await form.getByRole("combobox", { name: "已保存绩效评价", exact: true }).selectOption(id);
  await form.getByRole("button", { name: "读取固定绩效评价", exact: true }).click();
  const report = form.getByRole("article", { name: "固定评价报告", exact: true });
  await expect(report).toContainText(first.task.result_digest);
  await expect(report).toContainText(first.experiment.source.program_artifact);
  const stressView = report.getByRole("region", { name: "固定静态压力结果", exact: true });
  await expect(stressView).toContainText(first.experiment.request.static_stress.purpose);
  for (const [key, label] of [
    ["mark_change", "情景市值变化"],
    ["equity", "情景权益"],
    ["margin", "情景保证金占用"],
    ["available", "情景可用资金"],
    ["gap", "情景资金缺口"],
  ])
    await expect(stressView.getByText(label, { exact: true }).locator("+ dd")).toHaveText(
      `${first.result.static_stress[key]} ${first.result.currency}`,
    );
  const exposureView = report.getByRole("region", { name: "原末日敞口与资金切片", exact: true });
  await expect(exposureView).toContainText(last.trading_day);
  await expect(exposureView.getByText("原可用资金", { exact: true }).locator("+ dd")).toHaveText(
    `${last.occupancy.available} ${first.result.currency}`,
  );
  const exposureRows = exposureView
    .getByRole("table", { name: "原合约敞口与单位证据", exact: true })
    .getByRole("row");
  await expect(exposureRows).toHaveCount(exposure.positions.length + 1);
  for (let i = 0; i < exposure.positions.length; ++i) {
    const position = exposure.positions[i];
    await expect(
      exposureRows
        .nth(i + 1)
        .getByRole("cell")
        .nth(1),
    ).toContainText(`${position.venue}/${position.symbol}`);
    await expect(
      exposureRows
        .nth(i + 1)
        .getByRole("cell")
        .nth(2),
    ).toHaveText(position.price);
    await expect(
      exposureRows
        .nth(i + 1)
        .getByRole("cell")
        .nth(3),
    ).toHaveText(position.multiplier);
    await expect(
      exposureRows
        .nth(i + 1)
        .getByRole("cell")
        .nth(5),
    ).toHaveText(position.net_quantity);
    await expect(
      exposureRows
        .nth(i + 1)
        .getByRole("cell")
        .nth(6),
    ).toHaveText(position.gross_quantity ?? "未记录");
  }
  const dailyView = report.getByRole("region", { name: "原日采样损失路径", exact: true });
  await expect(
    dailyView.getByRole("img", { name: "当前页日采样回撤曲线", exact: true }),
  ).toBeVisible();
  const dailyRows = dailyView
    .getByRole("table", { name: "日采样原行证据", exact: true })
    .getByRole("row");
  await expect(dailyRows).toHaveCount(daily.points.length + 1);
  await expect(dailyRows.nth(1).getByRole("cell").nth(3)).toHaveText(
    `${daily.points[0].equity} ${first.result.currency}`,
  );
  const episodeRows = dailyView
    .getByRole("table", { name: "日采样回撤片段", exact: true })
    .getByRole("row");
  await expect(episodeRows).toHaveCount(daily.episodes.length + 1);
  for (let i = 0; i < daily.episodes.length; ++i)
    await expect(
      episodeRows
        .nth(i + 1)
        .getByRole("cell")
        .nth(2),
    ).toHaveText(
      daily.episodes[i].recovery_source_index === null
        ? "尚未恢复"
        : String(daily.episodes[i].recovery_source_index),
    );
  await expect(
    report.getByRole("list", { name: "评价未覆盖范围", exact: true }).getByRole("listitem"),
  ).toHaveCount(6);
  await form.getByLabel("年期数A（日结算）", { exact: true }).fill("250");
  await expect(report).toContainText(first.task.result_digest);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: sourceId })).task_result,
  ).toEqual(source);
  const state = await rpc(page.request, "runtime.snapshot");
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
    first,
  );
  await openView(page, "projects");
  for (const [label, value] of [
    ["项目标题", "固定绩效人工审阅"],
    ["研究问题", "这份固定评价能够说明什么？"],
    ["可检验假设", "合成绩效不能自动建立市场结论。"],
    ["支持标准", "需要独立证据，不能只看正收益。"],
    ["否定标准", "缺独立证据时明确记录证据不足。"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "保存研究草稿", exact: true }).click();
  await expect(workspace.getByText("草稿已保存", { exact: true })).toBeVisible();
  await workspace.getByLabel("已核对上方原文，固定后保留原标准", { exact: true }).check();
  await workspace.getByRole("button", { name: "固定计划", exact: true }).click();
  const manual = workspace.getByRole("region", { name: "人工研究结论", exact: true });
  await manual.getByLabel("正式研究任务身份", { exact: true }).fill(id);
  await manual.getByRole("button", { name: "读取待审阅正式结果", exact: true }).click();
  await expect(manual.getByRole("status")).toHaveText("回溯关联：此任务接纳时未固定所选计划");
  await manual.getByText("核对结论引用的原计划和正式结果", { exact: true }).click();
  await expect(manual.getByRole("article", { name: "固定评价报告", exact: true })).toContainText(
    first.task.result_digest,
  );
  await expect(manual.getByRole("button", { name: "追加人工结论", exact: true })).toBeDisabled();
  await manual.getByLabel("明确人工判断", { exact: true }).selectOption("insufficient");
  const reason = "人工事后审阅：合成固定绩效缺独立市场证据，不授权交易。";
  await manual.getByLabel("人工判断理由", { exact: true }).fill(reason);
  await manual.getByLabel("已核对精确计划和正式证据，明确保存人工判断", { exact: true }).check();
  await manual.getByRole("button", { name: "追加人工结论", exact: true }).click();
  await expect(manual.getByText(reason, { exact: true })).toBeVisible();
  const projects = (await rpc(page.request, "research.project.list", { after: "", limit: 30 }))
    .research_project.drafts;
  const project = projects.find(
    (row: { definition: { title: string } }) => row.definition.title === "固定绩效人工审阅",
  );
  expect(project).toBeTruthy();
  const human = (
    await rpc(page.request, "research.conclusion.list", {
      project_id: project.id,
      after: "",
      limit: 30,
    })
  ).research_project.conclusions[0];
  expect(human.retrospective).toBe(true);
  expect(human.declaration.result).toMatchObject({
    kind: "performance_evaluation_result",
    id,
    version: first.task.result_digest,
  });
  expect(human.program_artifact).toBe(first.task.program_artifact);
  const output = manual.getByRole("region", { name: "固定评价本地导出", exact: true });
  await expect(
    output.getByRole("button", { name: "导出固定评价报告", exact: true }),
  ).toBeDisabled();
  await expect(output).toContainText(human.digest);
  for (const format of ["html", "csv"]) {
    await output
      .getByRole("combobox", { name: "固定评价导出格式", exact: true })
      .selectOption(format);
    await expect(
      output.getByRole("button", { name: "导出固定评价报告", exact: true }),
    ).toBeDisabled();
    await output
      .getByLabel("已核对固定报告、人工解释范围及全部限制，明确导出本地文件", { exact: true })
      .check();
    const downloading = page.waitForEvent("download");
    await output.getByRole("button", { name: "导出固定评价报告", exact: true }).click();
    const downloaded = await downloading;
    expect(downloaded.suggestedFilename()).toBe(`${id}.evaluation.${format}`);
    const filename = await downloaded.path();
    expect(filename).toBeTruthy();
    const content = readFileSync(filename!, "utf8");
    expect(content).toContain(first.task.result_digest);
    expect(content).toContain(human.digest);
    expect(content).toContain(reason);
    expect(content).toContain("insufficient");
    for (const limitation of first.result.limitations) expect(content).toContain(limitation);
    expect(content).toContain("not a complete unified evaluation report");
    expect(content).toContain(String(first.result.net_profit));
    expect(content).toContain("exposure.funds.available");
    expect(content).toContain("stress.scenario.funds_model");
    expect(content).toContain("paper_balance_equity_minimum");
    expect(content).toContain("stress.result.mark_change");
    expect(content).toContain("stress.result.gap");
    for (const position of exposure.positions) {
      expect(content).toContain(
        `exposure.position.${position.source_contract_index}.gross_notional`,
      );
      expect(content).toContain(`exposure.position.${position.source_contract_index}.multiplier`);
    }
    for (const point of daily.points)
      expect(content).toContain(`daily.point.${point.source_index}.equity`);
    if (
      daily.episodes.some(
        (episode: { recovery_source_index: number | null }) =>
          episode.recovery_source_index === null,
      )
    )
      expect(content).toContain("NOT_RECOVERED");
    if (format === "html") expect(content).toContain("Original daily drawdown fractions");
  }
  const exportDirectory = mkdtempSync(join(tmpdir(), "asterion-evaluation-export-"));
  try {
    const path = join(exportDirectory, "fixed.html");
    const parameters = {
      id,
      owner: {
        kind: "research",
        node_id: state.research_service.node_id,
        service: state.research_service.service,
        connection_id: state.research_service.connection_id,
      },
      version: first.task.result_digest,
      format: "html",
      conclusion_id: human.declaration.id,
      conclusion_version: human.digest,
      path,
    };
    const receipt = (await rpc(page.request, "evaluation.report.export", parameters))
      .research_export;
    expect(receipt.path).toBe(path);
    expect(receipt.source_version).toBe(first.task.result_digest);
    const content = readFileSync(path, "utf8");
    expect(content).toContain(human.digest);
    const repeated = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "evaluation.report.export", params: parameters },
    });
    expect((await repeated.json()).error).toBeTruthy();
    expect(readFileSync(path, "utf8")).toBe(content);
    const wrong = await page.request.post("/__asterion/api", {
      data: {
        version: 1,
        method: "evaluation.report.export",
        params: {
          ...parameters,
          path: join(exportDirectory, "wrong.html"),
          conclusion_version: "f".repeat(64),
        },
      },
    });
    expect((await wrong.json()).error).toBeTruthy();
  } finally {
    rmSync(exportDirectory, { recursive: true, force: true });
  }

  expect((await taskRpc(page.request, "research", "task.result.read", { id })).task_result).toEqual(
    first,
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
    (await rpc(page.request, "research.conclusion.read", { id: human.declaration.id }))
      .research_project.conclusion,
  ).toEqual(human);
  await page.goto("/?screen=settings&category=resources");
  const catalogue = page.getByRole("region", { name: "业务资产目录", exact: true });
  await catalogue.getByLabel("资产所属域").selectOption("research");
  await catalogue.getByLabel("资产类别").selectOption("performance_evaluation_result");
  await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
  const entry = catalogue.getByRole("row").filter({ hasText: id });
  await expect(entry).toContainText(first.task.result_digest);
  await entry.getByRole("button", { name: "查看引用与维护", exact: true }).click();
  await expect(
    catalogue.getByRole("region", { name: "资源引用与维护预览", exact: true }),
  ).toContainText(first.task.result_digest);
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
  const temporary = mkdtempSync(join(tmpdir(), "asterion-evaluation-cold-"));
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
          row.object.kind === "performance_evaluation_result" &&
          row.object.id === id &&
          row.object.version === first.task.result_digest,
      ),
    ).toBe(true);

    expect(
      manifest.files.some(
        (row: { path: string; sha256: string }) =>
          row.path === `${id}/results/${first.task.attempt}.pb` &&
          row.sha256 === first.task.result_digest,
      ),
    ).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
