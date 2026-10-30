import { readFile } from "node:fs/promises";
import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";

// The R1 slice end to end inside the software: fixed one-minute data, a rule
// backtest with fees and slippage, its result and evidence, a performance and
// risk evaluation exported as a report, and a strategy package frozen from that
// result. Market data is the isolated test double; the steps in between are the
// product's own.
test(
  "R1: data, rule backtest, result and strategy package",
  { tag: "@journey" },
  async ({ page }) => {
    test.setTimeout(180000);
    page.setDefaultTimeout(15000);
    expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
    await page.goto("/");
    const selected = await seedDataset(
      page.request,
      [3500, 3502, 3506, 3510, 3504, 3498, 3494, 3500, 3507, 3512, 3503, 3495],
      "r1-minutes",
    );
    const dataset = selected.datasets[0];
    expect(dataset.interval_minutes).toBe(1);

    // A rule backtest with fees and one tick of slippage on the fixed data.
    await rpc(page.request, "backtest.submit", {
      id: "r1-backtest",
      strategies: [
        { quantity: "1", sides: "both", rule: { kind: "moving_average", fast: 1, slow: 3 } },
      ],
      holdout_from: "",
      walk_forward: null,
      deposit: "100000",
      max_order_quantity: "1",
      max_gross_quantity: "1",
      max_working_orders: "4",
      contracts: [
        {
          venue: dataset.venue,
          symbol: dataset.symbol,
          slippage_ticks: 1,
          cost_schedule: [
            {
              effective_from: "1970-01-01",
              source: "isolated explicit fixture costs",
              values: {
                margin_per_lot: "3500",
                margin_rate: "0",
                open_fee: "3",
                close_today_fee: "3",
                close_yesterday_fee: "3",
                open_fee_rate: "0",
                close_today_fee_rate: "0",
                close_yesterday_fee_rate: "0",
              },
            },
          ],
        },
      ],
    });
    await expect
      .poll(
        async () =>
          (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
            (task: { id: string }) => task.id === "r1-backtest",
          )?.state,
        { timeout: 30000 },
      )
      .toBe("succeeded");
    const result = (
      await taskRpc(page.request, "research", "task.result.read", { id: "r1-backtest" })
    ).task_result;
    expect(result.experiment.paper.contracts[0].slippage_ticks).toBe(1);

    // The result page shows the account and exports the fixed evidence.
    await openView(page, "backtest");
    const workspace = page.getByRole("region", { name: "期货研究", exact: true });
    await workspace
      .getByRole("region", { name: "研究任务", exact: true })
      .getByRole("row")
      .filter({ has: page.getByText("r1-backtest", { exact: true }) })
      .getByRole("button", { name: "查看结果", exact: true })
      .click();
    const shown = workspace.getByRole("region", { name: "回测结果", exact: true });
    await expect(shown).toContainText(result.result.account.equity);
    const downloading = page.waitForEvent("download");
    await shown.getByRole("button", { name: "导出研究证据", exact: true }).click();
    const evidence = JSON.parse(await readFile((await (await downloading).path())!, "utf8"));
    expect(evidence.result).toEqual(result.result);

    // Performance and risk from the same result, fixed as an evaluation and
    // exported as a local report.
    await openView(page, "risk");
    const risk = page.getByRole("region", { name: "风险", exact: true });
    await risk.getByRole("combobox", { name: "评估来源", exact: true }).selectOption("r1-backtest");
    await risk.getByRole("button", { name: "读取评估来源", exact: true }).click();
    const form = risk.getByRole("region", { name: "固定绩效评价", exact: true });
    const creation = form.locator("details.evaluation-create");
    if (!(await creation.evaluate(element => (element as HTMLDetailsElement).open)))
      await creation.getByText("新建正式评价", { exact: true }).click();
    await form.getByLabel("评价问题与用途", { exact: true }).fill("R1 规则回测的绩效与损失路径");
    await form.getByLabel("年期数A（日结算）", { exact: true }).fill("252");
    await form.getByLabel("明确采用零无风险年收益假设", { exact: true }).check();
    await form.getByLabel("保存日采样损失路径（不代表盘中最大风险）", { exact: true }).check();
    await form.getByLabel("保存本范围末日敞口和资金观察（非盘中峰值）", { exact: true }).check();
    await form.getByRole("button", { name: "计算固定绩效评价", exact: true }).click();
    let evaluationId = "";
    await expect
      .poll(
        async () => {
          const task = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
            (row: { kind: string }) => row.kind === "performance_evaluation",
          );
          if (task) evaluationId = task.id;
          return task?.state;
        },
        { timeout: 30000 },
      )
      .toBe("succeeded");
    const evaluated = (
      await taskRpc(page.request, "research", "task.result.read", { id: evaluationId })
    ).task_result;
    await form
      .getByRole("combobox", { name: "已保存绩效评价", exact: true })
      .selectOption(evaluationId);
    await form.getByRole("button", { name: "读取固定绩效评价", exact: true }).click();
    await expect(form.getByRole("article", { name: "固定评价报告", exact: true })).toContainText(
      evaluated.task.result_digest,
    );
    const output = form.getByRole("region", { name: "固定评价本地导出", exact: true });
    await output.getByLabel("已核对固定报告及全部限制，明确导出本地文件", { exact: true }).check();
    const reportDownload = page.waitForEvent("download");
    await output.getByRole("button", { name: "导出固定评价报告", exact: true }).click();
    const report = await readFile((await (await reportDownload).path())!, "utf8");
    expect(report).toContain(evaluated.task.result_digest);
    expect(report).toContain(String(evaluated.result.net_profit));

    // A strategy package frozen from that result keeps its rule, contract and
    // one-minute interval.
    await openView(page, "strategies");
    await workspace.getByLabel("来源回测任务", { exact: true }).fill("r1-backtest");
    await workspace.getByLabel("候选名称", { exact: true }).fill("R1 均线");
    await workspace.getByLabel("候选用途", { exact: true }).fill("仿真柜台运行");
    await workspace.getByLabel("候选限制", { exact: true }).fill("合成数据，不证明收益");
    await workspace.getByRole("button", { name: "读取固定回测假设", exact: true }).click();
    await workspace.getByRole("button", { name: "冻结策略候选", exact: true }).click();
    await expect(
      workspace.getByRole("region", { name: "固定候选定义", exact: true }),
    ).toBeVisible();
    const [candidate] = (
      await rpc(page.request, "research.strategy.list", { after: "", limit: 30 })
    ).research_strategy.packages;
    expect(candidate.definition.interval_minutes).toBe(1);
    expect(candidate.definition.contract).toMatchObject({
      venue: dataset.venue,
      symbol: dataset.symbol,
    });
    expect(candidate.definition.strategy).toEqual(result.experiment.strategies[0]);
  },
);
