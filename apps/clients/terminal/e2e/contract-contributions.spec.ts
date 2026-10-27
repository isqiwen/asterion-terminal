import { test, expect } from "./test";
import { restartServices, rpc, taskRpc, seedHistory } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("original multi-contract contributions keep real fills, account residual and fixed files through restart and joint restore", async ({
  page,
}) => {
  test.setTimeout(120000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const days = [
    "2026-09-21",
    "2026-09-22",
    "2026-09-23",
    "2026-09-24",
    "2026-09-25",
    "2026-09-28",
    "2026-09-29",
    "2026-09-30",
  ];
  const one = await seedHistory(
    page.request,
    [100, 102, 106, 110, 104, 98, 94, 100],
    "contribution-rb",
    { tablesOnly: true, dailyPrices: true, dailyDays: days },
  );
  const two = await seedHistory(
    page.request,
    [100, 99, 95, 91, 98, 103, 108, 101],
    "contribution-hc",
    { product: "hc", keep: true, tablesOnly: true, dailyPrices: true, dailyDays: days },
  );
  for (const [product, selection] of [
    ["rb", one],
    ["hc", two],
  ] as const)
    await rpc(page.request, "data.dataset.daily", {
      contract_id: `SHFE/${product}/2026-10`,
      daily: selection.daily,
      begin_day: days[0],
      end_day: days.at(-1)!,
    });
  await rpc(page.request, "backtest.submit", {
    id: "contribution-source",
    strategies: [{ quantity: "1", sides: "both", rule: { kind: "momentum", lookback: 2 } }],
    holdout_from: "",
    walk_forward: null,
    deposit: "100000",
    max_order_quantity: "1",
    max_gross_quantity: "2",
    max_working_orders: "4",
    contracts: ["rb2610", "hc2610"].map(symbol => ({
      venue: "SHFE",
      symbol,
      slippage_ticks: 0,
      cost_schedule: [
        {
          effective_from: "1970-01-01",
          source: "isolated explicit original costs",
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
    })),
  });
  const tasks = async () => (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  await expect
    .poll(
      async () =>
        (await tasks()).find((t: { id: string }) => t.id === "contribution-source")?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const original = (
    await taskRpc(page.request, "research", "task.result.read", { id: "contribution-source" })
  ).task_result;
  expect(original.experiment.paper.contracts).toHaveLength(2);
  expect(original.result.account.fills.length).toBeGreaterThan(0);
  expect(original.result.fill_days).toHaveLength(original.result.account.fills.length);
  await page.goto("/");
  await openView(page, "risk");
  const risk = page.getByRole("region", { name: "风险", exact: true });
  await risk
    .getByRole("combobox", { name: "评估来源", exact: true })
    .selectOption({ value: "contribution-source" });
  await risk.getByRole("button", { name: "读取评估来源", exact: true }).click();
  const panel = risk.getByRole("region", { name: "固定绩效评价", exact: true });
  const creation = panel.locator("details.evaluation-create");
  if (!(await creation.evaluate(element => (element as HTMLDetailsElement).open)))
    await creation.getByText("新建正式评价", { exact: true }).click();
  const option = panel.getByLabel("保存原合约贡献（仅完整账户、全部日线）", { exact: true });
  await expect(option).not.toBeChecked();
  await option.check();
  await panel
    .getByLabel("评价问题与用途", { exact: true })
    .fill("固定原合约与账户核对，不把未平仓当作闭合交易或授予交易权限");
  await panel.getByRole("button", { name: "计算固定绩效评价", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const task = (await tasks()).find(
          (t: { kind: string }) => t.kind === "performance_evaluation",
        );
        if (task) id = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const report = (await taskRpc(page.request, "research", "task.result.read", { id })).task_result;
  const contribution = report.result.contributions;
  expect(report.experiment.request.include_contributions).toBe(true);
  expect(report.experiment.request.scope).toBe("full_account");
  expect(contribution.contracts).toHaveLength(2);
  expect(contribution.reconciled).toBe(true);
  expect(contribution.residual).toBe("0");
  expect(contribution.fees).toBe(original.result.account.fees);
  const units = (value: string) => {
    const negative = value.startsWith("-");
    const [whole, fraction = ""] = (negative ? value.slice(1) : value).split(".");
    return (negative ? -1n : 1n) * (BigInt(whole) * 100000000n + BigInt(fraction.padEnd(8, "0")));
  };
  expect(units(contribution.recorded_net)).toBe(
    units(original.result.account.equity) - units(original.experiment.paper.deposit),
  );
  let sum = 0n;
  const originalFills = new Map<
    string,
    { symbol: string; venue: string; price: string; quantity: string; fee: string }
  >();
  for (const fill of original.result.account.fills) originalFills.set(fill.id, fill);
  const seen = new Set<string>();
  for (const contract of contribution.contracts) {
    sum += units(contract.net);
    for (const row of contract.days)
      for (const fill of row.fills) {
        const source = originalFills.get(fill.id)!;
        expect(source).toBeTruthy();
        expect(seen.has(fill.id)).toBe(false);
        seen.add(fill.id);
        expect(contract.symbol).toBe(source.symbol);
        expect(contract.venue).toBe(source.venue);
        expect(fill.price).toBe(source.price);
        expect(fill.quantity).toBe(source.quantity);
        expect(fill.fee).toBe(source.fee);
      }
  }
  expect(seen.size).toBe(originalFills.size);
  expect(sum).toBe(units(contribution.explained_net));
  for (let i = 0; i < contribution.days.length; ++i) {
    expect(contribution.days[i].recorded_balance).toBe(original.result.settlements[i].balance);
    expect(contribution.days[i].recorded_equity).toBe(original.result.settlements[i].equity);
  }
  await panel
    .getByRole("combobox", { name: "已保存绩效评价", exact: true })
    .selectOption({ value: id });
  await panel.getByRole("button", { name: "读取固定绩效评价", exact: true }).click();
  const view = panel.getByRole("region", { name: "固定原合约贡献", exact: true });
  await expect(view.getByRole("status")).toContainText("账本金额核对一致");
  const totals = view.getByRole("table", { name: "全部原合约贡献合计", exact: true });
  await expect(totals.locator("tbody tr")).toHaveCount(2);
  for (let i = 0; i < contribution.contracts.length; ++i) {
    const c = contribution.contracts[i];
    await expect(totals).toContainText(c.symbol);
    await expect(totals).toContainText(c.net);
    await view
      .getByRole("combobox", { name: "查看原贡献合约", exact: true })
      .selectOption({ value: String(i) });
    const day = c.days.find((r: { fills: unknown[] }) => r.fills.length);
    expect(day).toBeTruthy();
    await view
      .getByRole("table", { name: "原合约逐日贡献", exact: true })
      .getByRole("row")
      .filter({ hasText: day.trading_day })
      .getByRole("button", { name: "查看原成交与核对项", exact: true })
      .click();
    await expect(view.getByRole("table", { name: "所选合约原成交", exact: true })).toContainText(
      day.fills[0].id,
    );
  }
  const output = panel.getByRole("region", { name: "固定评价本地导出", exact: true });
  await expect(output).toContainText("页面筛选和分页不缩小固定账户范围");
  const context = (await rpc(page.request, "runtime.snapshot")).research_service;
  const temporary = mkdtempSync(join(tmpdir(), "asterion-contributions-browser-"));
  try {
    for (const format of ["html", "csv"]) {
      await output
        .getByRole("combobox", { name: "固定评价导出格式", exact: true })
        .selectOption({ value: format });
      await output
        .getByLabel("已核对固定报告及全部限制，明确导出本地文件", { exact: true })
        .check();
      const downloading = page.waitForEvent("download");
      await output.getByRole("button", { name: "导出固定评价报告", exact: true }).click();
      const download = await downloading;
      expect(download.suggestedFilename()).toBe(`${id}.evaluation.${format}`);
      const content = readFileSync((await download.path())!, "utf8");
      const fields: [string, string][] = [
        ["scope", "full_account"],
        ["report.result_sha256", report.task.result_digest],
      ];
      for (const key of ["gross", "fees", "explained_net", "recorded_net", "residual"])
        fields.push([`contributions.${key}`, contribution[key]]);
      for (let c = 0; c < contribution.contracts.length; ++c) {
        const contract = contribution.contracts[c];
        for (const key of ["venue", "symbol", "gross", "fees", "net"])
          fields.push([`contributions.contract.${c}.${key}`, contract[key]]);
        for (let d = 0; d < contract.days.length; ++d) {
          const day = contract.days[d];
          for (const key of ["gross", "fees", "settlement"])
            fields.push([`contributions.contract.${c}.day.${d}.${key}`, day[key]]);
          for (let f = 0; f < day.fills.length; ++f)
            for (const key of ["id", "order_id", "quantity", "price", "fee"])
              fields.push([
                `contributions.contract.${c}.day.${d}.fill.${f}.${key}`,
                day.fills[f][key],
              ]);
        }
      }
      for (let d = 0; d < contribution.days.length; ++d)
        for (const key of [
          "recorded_balance",
          "recorded_equity",
          "expected_balance",
          "cumulative_residual",
        ])
          fields.push([`contributions.day.${d}.${key}`, contribution.days[d][key]]);
      for (const [field, value] of fields) {
        if (format === "csv") expect(content).toContain(`"${field}","${value}","RECORDED"`);
        else expect(content).toContain(`<th>${field}</th><td>${value}</td><td>RECORDED</td>`);
      }
      const path = join(temporary, `original.${format}`);
      const parameters = {
        id,
        owner: {
          kind: "research",
          node_id: context.node_id,
          service: context.service,
          connection_id: context.connection_id,
        },
        version: report.task.result_digest,
        format,
        path,
      };
      const receipt = (await rpc(page.request, "evaluation.report.export", parameters))
        .research_export;
      expect(receipt.source_version).toBe(report.task.result_digest);
      expect(readFileSync(path, "utf8")).toBe(content);
      const repeated = await page.request.post("/__asterion/api", {
        data: { version: 1, method: "evaluation.report.export", params: parameters },
      });
      expect((await repeated.json()).error).toBeTruthy();
      expect(readFileSync(path, "utf8")).toBe(content);
      const wrongPath = join(temporary, `wrong.${format}`);
      const wrong = await page.request.post("/__asterion/api", {
        data: {
          version: 1,
          method: "evaluation.report.export",
          params: { ...parameters, version: "0".repeat(64), path: wrongPath },
        },
      });
      expect((await wrong.json()).error).toBeTruthy();
      expect(existsSync(wrongPath)).toBe(false);
    }
    await restartServices(page.request, [context.service], async () => {
      await rpc(page.request, "node.action", {
        id: "local",
        service: context.service,
        action: "stop",
      });
      await rpc(page.request, "node.action", {
        id: "local",
        service: context.service,
        action: "start",
      });
    });
    expect(
      (await taskRpc(page.request, "research", "task.result.read", { id })).task_result,
    ).toEqual(report);
    expect(
      (await taskRpc(page.request, "research", "task.result.read", { id: "contribution-source" }))
        .task_result,
    ).toEqual(original);
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
    const originalBytes = new Map<string, Buffer>();
    for (const task of [id, "contribution-source"])
      originalBytes.set(
        task,
        readFileSync(join(backup, "owners", String(owner), task, "results", "1.pb")),
      );
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
    for (const [task, bytes] of originalBytes)
      expect(
        readFileSync(join(restored, "owners", String(owner), task, "results", "1.pb")),
      ).toEqual(bytes);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
