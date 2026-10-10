import { seedDataset } from "./dataset-fixture";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";
import { join } from "node:path";

test(
  "Agent runs a bar backtest and restores its evidence",
  { tag: "@journey" },
  async ({ page }) => {
    await page.goto("/");
    await expect(page.locator(".workspace-tabs")).toBeVisible();
    const seeded = await seedDataset(
      page.request,
      [100, 101, 102, 101, 104, 103, 102, 103],
      "task",
    );
    await page.reload();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    const workspace = page.getByRole("region", { name: "期货研究", exact: true });
    await expect(workspace.getByText("任务服务已连接", { exact: true })).toBeVisible();
    await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
    const selected = workspace.getByRole("list", { name: "已选合约" });
    await expect(selected).toContainText("SHFE · rb2610");
    await expect(selected).toContainText("8 根 · 1 个交易日");
    await workspace.getByRole("button", { name: "下一步", exact: true }).click();
    for (const [label, value] of [
      ["快均线", "1"],
      ["慢均线", "3"],
      ["目标手数", "1"],
      ["初始资金", "10000"],
      ["每手保证金", "100"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
      ["单笔数量上限", "100"],
      ["总持仓量上限", "100"],
      ["在途委托数上限", "100"],
    ])
      await workspace.getByLabel(label, { exact: true }).fill(value);
    // Drafts survive switching workspaces.
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await expect(workspace.getByLabel("快均线", { exact: true })).toHaveValue("1");
    await expect(workspace.getByLabel("初始资金", { exact: true })).toHaveValue("10000");
    const taskIds = () =>
      workspace
        .getByRole("region", { name: "研究任务", exact: true })
        .locator("tbody tr code")
        .allTextContents();
    await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
    const before = new Set(await taskIds());
    await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
    await workspace.getByRole("button", { name: "下一步", exact: true }).click();
    await workspace.getByRole("button", { name: "下一步", exact: true }).click();
    await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
    await expect(
      workspace.getByText("任务已提交，可在任务中心查看进度。", { exact: true }),
    ).toBeVisible();
    await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
    await expect.poll(async () => (await taskIds()).filter(id => !before.has(id)).length).toBe(1);
    const taskId = (await taskIds()).find(id => !before.has(id))!;
    await page.reload();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    const row = workspace
      .getByRole("region", { name: "研究任务", exact: true })
      .getByRole("row")
      .filter({ hasText: taskId });
    await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
    // A full reload starts a fresh draft scope; persisted task results remain available.
    await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
    await workspace.getByRole("button", { name: "下一步", exact: true }).click();
    await expect(workspace.getByLabel("初始资金", { exact: true })).toHaveValue("");
    await workspace.getByLabel("快均线", { exact: true }).fill("9");
    await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
    // Each mode lists its own tasks; the factor pages do not show this one.
    await page.getByRole("button", { name: "因子", exact: true }).click();
    await expect(row).toHaveCount(0);
    await page.getByRole("button", { name: "回测", exact: true }).click();
    await row.getByRole("button", { name: "查看结果", exact: true }).click();
    await expect(page.getByRole("button", { name: "回测", exact: true })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    const result = workspace.getByRole("region", { name: "回测结果", exact: true });
    // One trading day has a return and a drawdown, and too little for yearly figures.
    const performance = result.getByRole("region", { name: "绩效", exact: true });
    await expect(performance).toBeVisible();
    const figure = (label: string) =>
      performance
        .locator("div")
        .filter({ has: page.getByText(label, { exact: true }) })
        .last();
    await expect(figure("交易日数")).toContainText("1");
    await expect(figure("总收益率")).toContainText(/^总收益率-?\d+\.\d{2}%$/);
    await expect(figure("夏普比率")).toContainText("—");
    await performance.screenshot({
      path: join(__dirname, "../test-results/backtest-performance.png"),
    });
    await expect(result.getByRole("img", { name: "权益曲线" })).toBeVisible();
    const availability = result.getByRole("region", { name: "历史可知性", exact: true });
    await expect(availability).toContainText("来源公布时间未知");
    await availability.getByText("版本取得证据", { exact: true }).click();
    await expect(availability.locator("code")).toHaveText(
      [
        ...seeded.datasets[0].source_dataset_ids,
        ...seeded.datasets[0].settlement_dataset_ids,
      ].sort(),
    );
    await expect(availability).toContainText("完整版本取得时间");
    await result.getByText("实验参数", { exact: true }).click();
    const parameter = (label: string) =>
      result
        .locator(".experiment-fields > div")
        .filter({ has: page.getByText(label, { exact: true }) })
        .locator("dd");
    await expect(parameter("快均线")).toHaveText("1");
    // The form starts on both sides; the experiment records what was submitted.
    await expect(parameter("持仓方向")).toHaveText("多空");
    await expect(parameter("初始资金")).toHaveText("10000");
    await expect(parameter("平今手续费")).toHaveText("3");
    await expect(parameter("输入 K 线")).toHaveText("8");
    await expect(parameter("K 线数据版本")).toHaveText(seeded.datasets[0].source_dataset_ids[0]);
    await expect(parameter("结算价数据版本")).toHaveText(
      seeded.datasets[0].settlement_dataset_ids[0],
    );
    await expect(parameter("交易日范围")).toHaveText("2026-09-25 – 2026-09-25");
    await result.getByText("逐日结算", { exact: true }).click();
    const settlements = result.locator(".research-settlements tbody tr");
    await expect(settlements).toHaveCount(1);
    await expect(settlements.first()).toContainText("110");
    // The page is named for what it does, the result for the rule it ran.
    await expect(workspace.getByRole("heading", { name: "策略回测", exact: true })).toBeVisible();
    await expect(workspace.getByRole("heading", { name: /均线交叉 1\/3/ })).toBeVisible();
    await result
      .locator(".research-experiment")
      .screenshot({ path: join(__dirname, "../test-results/backtest-factor-evidence.png") });
    await page.screenshot({ path: join(__dirname, "../test-results/backtest-result.png") });
    await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
    await page.setViewportSize({ width: 800, height: 900 });
    await expect(
      workspace.getByRole("button", { name: "查看结果", exact: true }).last(),
    ).toBeVisible();
    expect(
      await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth),
    ).toBe(true);
    // Renderer-only capacity fixture; the C++ suite separately runs 200000 bars.
    // Do not pass the full curve as function arguments (V8 throws RangeError).
    await page.route("**/__asterion/api", async route => {
      const method = route.request().postDataJSON()?.method;
      // Polls must expose the same renderer fixture as the result command.
      if (method !== "task.result" && method !== "runtime.snapshot") return route.continue();
      const response = await route.fetch();
      const body = await response.json();
      const backtest = body.result?.task_result;
      if (backtest?.kind === "backtest") {
        const sample = backtest.result.equity[0];
        backtest.result.equity = Array.from({ length: 200001 }, (_, i) => ({
          ...sample,
          timestamp_ns: String(1790298000000000000n + BigInt(i) * 1000000n),
          equity: String(10000 + (i % 101)),
        }));
      }
      await route.fulfill({ response, json: body });
    });
    await row.getByRole("button", { name: "查看结果", exact: true }).click();
    await expect(result.getByRole("img", { name: "权益曲线" })).toBeVisible();
    await expect
      .poll(async () =>
        result
          .locator("polyline")
          .evaluate(element => element.getAttribute("points")?.trim().split(/\s+/).length),
      )
      .toBe(200001);
    // A polling response may still be expanding the large chart fixture. Drain
    // those handlers before navigation removes their routes or closes the page.
    await page.unrouteAll({ behavior: "wait" });
    page = await openSettingsWindow(page);
    await page.getByLabel("语言", { exact: true }).selectOption("en-US");
    page = await closeSettingsWindow(page);
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "Research", exact: true })
      .click();
    const english = page.getByRole("region", { name: "Backtest Result", exact: true });
    // The parameters <details> opened in Chinese stays open across the
    // language switch; clicking it again would collapse it.
    if ((await english.locator("details.research-experiment").getAttribute("open")) === null)
      await english.getByText("Experiment Parameters", { exact: true }).click();
    await expect(english).not.toContainText(/\p{Script=Han}/u);
    await expect(english.getByText("Input bars", { exact: true })).toBeVisible();
  },
);

test("a portfolio backtest settles every contract on one account", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  const prices = [100, 101, 102, 101, 104, 103, 102, 103];
  await seedDataset(page.request, prices, "portfolio-rb");
  await seedDataset(page.request, prices, "portfolio-hc", { product: "hc", keep: true });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await page.getByRole("button", { name: "回测", exact: true }).click();
  await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
  const selected = workspace.getByRole("list", { name: "已选合约" });
  await expect(selected.getByRole("listitem")).toHaveCount(2);
  await expect(selected).toContainText("SHFE · hc2610");
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  for (const symbol of ["rb2610", "hc2610"]) {
    const costs = workspace.getByRole("region", { name: `SHFE · ${symbol} 保证金与手续费` });
    for (const [label, value] of [
      ["每手保证金", "100"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
    ])
      await costs.getByLabel(label, { exact: true }).fill(value);
  }
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  await expect(
    workspace.getByText("任务已提交，可在任务中心查看进度。", { exact: true }),
  ).toBeVisible();
  await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
  const row = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/rb2610 + SHFE/hc2610" });
  await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await row.getByRole("button", { name: "查看结果", exact: true }).first().click();
  const result = workspace.getByRole("region", { name: "回测结果", exact: true });
  await result.getByText("逐日结算", { exact: true }).click();
  const settlements = result.locator(".research-settlements tbody tr");
  await expect(settlements).toHaveCount(2);
  await expect(settlements.nth(0)).toContainText("rb2610");
  await expect(settlements.nth(1)).toContainText("hc2610");
  await result.getByText("实验参数", { exact: true }).click();
  await expect(result.getByRole("region", { name: "SHFE · hc2610" })).toContainText("100");
  // Two contracts are neither one series nor a cross-section; the factor page says so.
  await page.getByRole("button", { name: "因子", exact: true }).click();
  await expect(
    workspace.getByText("比较合约至少需要 3 个；只分析一个合约时请只保留一个数据集。", {
      exact: true,
    }),
  ).toBeVisible();
  await workspace.getByRole("button", { name: "移除 SHFE · hc2610", exact: true }).click();
  await expect(selected.getByRole("listitem")).toHaveCount(1);
});

test("a backtest trades by the rule chosen in the form", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  await seedDataset(page.request, [100, 101, 102, 101, 100, 101, 103, 103], "momentum");
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  // Each rule shows its own windows and says what it does with them.
  await workspace.getByLabel("策略", { exact: true }).selectOption("momentum");
  await expect(workspace.getByLabel("快均线", { exact: true })).toHaveCount(0);
  await expect(workspace.getByText(/收盘价高于“动量回看”根 K 线之前的收盘价时做多/)).toBeVisible();
  for (const [label, value] of [
    ["动量回看", "2"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["每手保证金", "100"],
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
  await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
  const row = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/rb2610" })
    .first();
  await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await row.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "回测结果", exact: true });
  await expect(result.getByRole("heading", { name: /时序动量 2/ })).toBeVisible();
  await result.getByText("实验参数", { exact: true }).click();
  const parameter = (label: string) =>
    result
      .locator(".experiment-fields > div")
      .filter({ has: page.getByText(label, { exact: true }) })
      .locator("dd");
  await expect(parameter("策略")).toHaveText("时序动量");
  await expect(parameter("动量回看")).toHaveText("2");
  await expect(parameter("持仓方向")).toHaveText("多空");
  await page.screenshot({ path: join(__dirname, "../test-results/backtest-rule-form.png") });
});

test("several strategies are compared before a holdout and one is replayed", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // Thirty weekdays from 2026-08-17 with one bar each: a rising market with a
  // dip every third day.
  const days: string[] = [];
  for (let at = Date.UTC(2026, 7, 17); days.length < 30; at += 86400000)
    if (new Date(at).getUTCDay() % 6 !== 0) days.push(new Date(at).toISOString().slice(0, 10));
  await seedDataset(
    page.request,
    days.map((_, i) => 100 + 2 * i - (i % 3 === 2 ? 3 : 0)),
    "comparison",
    { minuteDays: days },
  );
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByLabel("快均线", { exact: true }).fill("1");
  // One value is one strategy and asks for no holdout.
  await workspace.getByLabel("慢均线", { exact: true }).fill("2");
  await expect(workspace.getByText("一个策略，不做比较。", { exact: true })).toBeVisible();
  await expect(workspace.getByLabel("留出起始日", { exact: true })).toHaveCount(0);
  // A fast average that is not shorter than the slow one is no strategy.
  await workspace.getByLabel("慢均线", { exact: true }).fill("1");
  await expect(workspace.getByText("这些窗口组不出可用的策略。", { exact: true })).toBeVisible();
  await expect(workspace.getByRole("button", { name: "下一步", exact: true })).toBeDisabled();
  await workspace.getByLabel("慢均线", { exact: true }).fill("2, 3");
  await expect(workspace.getByText(/^候选策略 2 个/)).toBeVisible();
  // A Saturday: the holdout begins on the Monday after it.
  await workspace.getByLabel("留出起始日", { exact: true }).fill("2026-09-19");
  for (const [label, value] of [
    ["目标手数", "1"],
    ["初始资金", "100000"],
    ["每手保证金", "100"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(workspace.getByText(/2 个候选，留出自 2026-09-19 起/)).toBeVisible();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
  const row = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/rb2610" })
    .first();
  await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await row.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "回测结果", exact: true });
  // Both candidates keep their scores; the heading names the selected one.
  const candidates = result.getByRole("table", { name: "候选策略", exact: true });
  await expect(candidates.locator("tbody tr")).toHaveCount(2);
  await expect(candidates.locator("tbody tr").nth(0)).toContainText("均线交叉 1/2");
  await expect(candidates.locator("tbody tr").nth(1)).toContainText("均线交叉 1/3");
  const chosen = candidates.locator("tbody tr").filter({ hasText: "已选中" });
  await expect(chosen).toHaveCount(1);
  const rule = (await chosen.locator("td").first().innerText()).split(" · ")[0];
  await expect(result.getByRole("heading", { name: new RegExp(rule) })).toBeVisible();
  const figure = (section: string, name: string) =>
    result
      .getByRole("region", { name: section, exact: true })
      .locator(".research-metrics > div")
      .filter({ has: page.getByText(name, { exact: true }) })
      .locator("strong");
  // Twenty-five days before 2026-09-21 and five from it; the whole run has both.
  await expect(figure("绩效", "交易日数")).toHaveText("30");
  await expect(figure("前段", "交易日数")).toHaveText("25");
  await expect(figure("留出段", "交易日数")).toHaveText("5");
  // The development figures are the selected candidate's scores.
  await expect(figure("前段", "总收益率")).toHaveText(
    await chosen.locator("td").nth(1).innerText(),
  );
  await expect(figure("前段", "夏普比率")).toHaveText(
    await chosen.locator("td").nth(2).innerText(),
  );
  // Five days are too few for yearly figures.
  await expect(figure("留出段", "夏普比率")).toHaveText("—");
  await result.getByRole("heading", { name: "比较", exact: true }).scrollIntoViewIfNeeded();
  await page.screenshot({ path: join(__dirname, "../test-results/backtest-comparison.png") });
  await result.getByText("实验参数", { exact: true }).click();
  const parameter = (label: string) =>
    result
      .locator(".experiment-fields > div")
      .filter({ has: page.getByText(label, { exact: true }) })
      .locator("dd");
  await expect(parameter("候选策略数")).toHaveText("2");
  await expect(parameter("留出起始日")).toHaveText("2026-09-21");
});
