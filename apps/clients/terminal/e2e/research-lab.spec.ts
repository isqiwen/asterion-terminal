import { execFileSync } from "node:child_process";
import { join } from "node:path";
import { openView } from "./navigation-helper";
import { test, expect, type Page } from "./test";

const source = join(__dirname, "../../../..");

// Lab's fixture warehouse in the independent fixture Warehouse directory, once
// for the tests of this file.
let prepared = false;
function prepare() {
  if (prepared) return;
  prepared = true;
  execFileSync(join(source, "build/electron-resources/python/bin/python3"), [
    "-I",
    "-B",
    join(source, "tests/lab/warehouse_fixture.py"),
    join(source, "apps/services/lab"),
    join(process.env.ASTERION_TEST_WAREHOUSE_TLS!, "../../data"),
  ]);
}

async function backtest(page: Page, fast: string, slow: string) {
  const form = page.locator(".lab-research:visible");
  await form.getByLabel("fast").fill(fast);
  await form.getByLabel("slow").fill(slow);
  await form.getByRole("button", { name: "运行", exact: true }).click();
  await expect(page.getByRole("heading", { name: "月度收益" })).toBeVisible({ timeout: 30000 });
}

test(
  "a strategy from the repository is configured, run, read and compared",
  { tag: "@journey" },
  async ({ page }) => {
    test.setTimeout(90000); // Includes the service's 60 s report budget.
    prepare();
    await page.goto("/");
    await openView(page, "research");
    await expect(
      page.getByRole("button", { name: "RandomFeatureForecast", exact: true }),
    ).toBeVisible({ timeout: 15000 });
    await page.getByRole("button", { name: "设置回测条件", exact: true }).click();
    const scope = page.getByRole("dialog", { name: "回测条件" });
    await scope.getByRole("combobox", { name: "品种" }).fill("SHFE/rb");
    await scope.getByRole("combobox", { name: "品种" }).press("Escape");
    await scope.getByLabel("开始日").fill("2026-01-05");
    await scope.getByLabel("结束日").fill("2026-02-13");
    await scope.getByLabel("单笔手数").fill("10");
    await scope.getByLabel("单合约持仓手数").fill("10");
    await scope.getByLabel("账户总持仓手数").fill("10");
    await scope.getByRole("button", { name: "完成", exact: true }).click();
    await page.getByRole("button", { name: "MaCross", exact: true }).click();

    await backtest(page, "2", "4");
    // Leaving full screen keeps the whole equity curve in view.
    const equity = page.getByRole("img", { name: "权益（逐日结算）", exact: true });
    const windowed = page.viewportSize()!;
    for (const size of [{ width: 3008, height: 1692 }, windowed]) {
      await page.setViewportSize(size);
      await page.evaluate(
        () => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done))),
      );
      await expect(equity).toHaveAttribute("data-visible-count", "29");
    }
    await expect(page.getByText(/^成交（[1-9]\d* 笔）$/)).toBeVisible();
    await expect(page.getByText("K 线与买卖点")).toBeVisible();
    await page.getByText("归因与诊断").click();
    await expect(page.getByText(/^QuantStats \d/)).toBeVisible({ timeout: 65000 });
    await page.screenshot({ path: join(__dirname, "../test-results/research-lab-result.png") });

    // The configuration stays live alongside the report on the strategy view.
    await backtest(page, "3", "6");
    await page.getByRole("button", { name: "在回测中打开", exact: true }).click();
    await page.getByRole("button", { name: "← 回测记录", exact: true }).click();
    const runs = page.getByRole("table", { name: "回测记录" }).locator("tbody tr");
    await expect(runs).toHaveCount(2);
    await expect(runs.locator(".status-badge")).toHaveText(["已完成", "已完成"]);
    for (const box of await page.getByRole("checkbox", { name: /^比较 / }).all()) await box.check();
    await page.getByRole("button", { name: "比较所选（2）" }).click();
    const comparison = page.locator(".lab-runs-view table.lab-compare");
    await expect(
      comparison.getByRole("columnheader", { name: /^MaCross \d\d-\d\d \d\d:\d\d:\d\d$/ }),
    ).toHaveCount(2);
    await expect(comparison.getByRole("row", { name: /^总收益 / })).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/research-lab-comparison.png") });
  },
);

test(
  "a factor from the repository is evaluated across symbols",
  { tag: "@journey" },
  async ({ page }) => {
    prepare();
    await page.goto("/");
    await openView(page, "factors");
    const library = page.getByRole("table", { name: "因子库" });
    await expect(
      library.getByRole("button", { name: "expected_shortfall", exact: true }),
    ).toBeVisible({ timeout: 15000 });
    await page.getByRole("button", { name: "设置评价条件", exact: true }).click();
    const scope = page.getByRole("dialog", { name: "评价条件" });
    await scope
      .getByRole("combobox", { name: "品种" })
      .fill("SHFE/rb/2026-05, SHFE/rb/2026-10, SHFE/rb");
    await scope.getByRole("combobox", { name: "品种" }).press("Escape");
    await scope.getByLabel("开始日").fill("2026-01-05");
    await scope.getByLabel("结束日").fill("2026-02-13");
    await scope.getByLabel("分组数").fill("3");
    await scope.getByRole("button", { name: "完成", exact: true }).click();
    await library
      .locator("tr", { has: page.getByLabel("选择 price.momentum", { exact: true }) })
      .getByRole("button", { name: "momentum", exact: true })
      .click();
    const form = page.locator(".lab-config");
    await form.getByLabel("lookback").fill("2");
    await form.getByRole("button", { name: "评价", exact: true }).click();
    const decay = page.getByRole("table", { name: "IC 与衰减" });
    await expect(decay.locator("tbody tr")).toHaveCount(6);
    await expect(decay.locator("tbody tr").first()).toContainText("1 根");
    await expect(page.getByRole("table", { name: "分组" }).locator("tbody tr")).toHaveCount(3);
    await expect(page.getByText(/3 个品种/)).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/research-lab-factor.png") });
    // The evaluation is kept under the factor's settings and reopens from there.
    await page.locator(".lab-page-head").getByRole("button", { name: "代码", exact: true }).click();
    const records = page.getByRole("list", { name: "评价记录" }).getByRole("button");
    await expect(records).toHaveCount(1);
    await records.click();
    await expect(decay).toBeVisible();
    await page.screenshot({
      path: join(__dirname, "../test-results/research-lab-factor-record.png"),
    });
  },
);
