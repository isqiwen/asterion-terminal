import { readFile } from "node:fs/promises";
import { test, expect } from "./test";
import { ctpConnection, rpc, seedDataset, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { checkSnapshot } from "./snapshot-schema";

// The R1 slice end to end inside the software: fixed one-minute data, a rule
// backtest with fees and slippage, its result and evidence, a strategy package
// frozen from that result, and a run of the same package on the simulated
// counter. Market data, the counter and the account are the isolated test
// doubles; the steps in between are the product's own.
test(
  "R1: data, rule backtest, result, strategy package and a simulated-counter run of that package",
  { tag: "@journey" },
  async ({ page }) => {
    test.setTimeout(180000);
    page.setDefaultTimeout(15000);
    expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
    const account = "r1-account";
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

    // The simulated counter account: created with its risk policy, connected
    // and authorized.
    await ctpConnection(page.request, account, {
      broker_id: "9999",
      user_id: "000001",
      trade_front: "tcp://127.0.0.1:41205",
    });
    await rpc(page.request, "market.local");
    await rpc(page.request, "market.catalog", {
      account,
      password: "catalog-only",
      auth_code: "auth-code",
    });
    const snapshot = () => rpc(page.request, "runtime.snapshot");
    await expect
      .poll(async () => (await snapshot()).market?.catalog?.phase, { timeout: 20000 })
      .toBe("ready");
    await rpc(page.request, "live.create", {
      account,
      node: "local",
      service: "",
      environment: "simulation",
      max_order_quantity: "1",
      max_gross_quantity: "1",
      max_working_orders: "2",
      max_price_deviation: "0.02",
      contracts: [{ venue: dataset.venue, symbol: dataset.symbol }],
    });
    const session = async () => (await snapshot()).live[account]?.session;
    await expect
      .poll(async () => (await session())?.phase, { timeout: 20000 })
      .toBe("disconnected");
    await rpc(page.request, "live.connect", {
      account,
      password: "e2e-r1-password",
      auth_code: "auth-code",
    });
    await expect.poll(async () => (await session())?.phase, { timeout: 20000 }).toBe("ready");
    const ready = await session();
    await rpc(page.request, "live.act", {
      account,
      request_id: crypto.randomUUID(),
      account_id: ready.account_id,
      policy_revision: ready.policy_revision,
      action: "live_authorize",
      user_id: "000001",
    });

    // The same package runs on the account. The test market service observes
    // nothing, so the run ends at once and says why; it never invents bars.
    await page.goto("/");
    await openView(page, "trading");
    const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
    await panel
      .getByRole("navigation", { name: "CTP 账户" })
      .getByRole("button", { name: new RegExp(`^${account}`) })
      .click();
    // A connected account in a window that has not confirmed its counter
    // environment offers only that step; confirming it removes the step.
    await panel
      .getByRole("region", { name: "柜台环境", exact: true })
      .getByLabel("我已核对账户与柜台环境", { exact: true })
      .click();
    await expect(panel.getByText("CTP 仿真 · 用户确认", { exact: true })).toBeVisible();
    await panel.getByRole("button", { name: "策略运行", exact: true }).click();
    const start = panel.getByRole("form", { name: "启动策略" });
    await expect(start.getByLabel("运行的策略包", { exact: true })).toHaveValue(
      candidate.object.id,
    );
    await expect(start.getByTestId("strategy-package-rule")).toContainText("1/3");
    await start.getByRole("button", { name: "启动策略", exact: true }).click();
    await expect(start.getByTestId("strategy-ended")).toContainText("行情服务还没有该合约的行情", {
      timeout: 15000,
    });
    const ended = await snapshot();
    checkSnapshot("R1 strategy run of the frozen package", ended);
    expect(ended.live[account].session!.strategy).toMatchObject({
      state: "stopped",
      venue: dataset.venue,
      symbol: dataset.symbol,
      package: candidate.object,
      strategy: candidate.definition.strategy,
      orders: [],
    });
    await rpc(page.request, "live.disconnect", { account });
  },
);
