import { seedHistory } from "./dataset-fixture";
import { removeFolder } from "./cleanup";
import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("paper trading uses C++ ledger and restores persisted account", async ({ page }) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-paper-e2e-"));
  const directory = join(folder, "account");
  await mkdir(directory);
  try {
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
      timeout: 60000,
    });
    await seedHistory(page.request, [100, 99, 110], "paper");
    await page.reload();
    await page.getByRole("button", { name: "交易", exact: true }).click();
    // Bars and settlement come from completed downloads; the contract units
    // are the only specification typed here.
    const picker = page.getByRole("form", { name: "历史数据集" });
    await picker.getByLabel("K 线来源", { exact: true }).selectOption("paper-bars");
    await expect(picker.getByLabel("结算价来源", { exact: true })).toHaveValue("paper-settlement");
    await picker.getByLabel("最小变动价位", { exact: true }).fill("1");
    await picker.getByLabel("合约乘数", { exact: true }).fill("10");
    await picker.screenshot({ path: join(__dirname, "../test-results/dataset-picker.png") });
    await picker.getByRole("button", { name: "使用此数据集", exact: true }).click();
    const selected = page.getByRole("region", { name: "历史数据集" });
    await expect(selected).toContainText("SHFE · rb2610");
    await expect(selected).toContainText("3 根 · 1 个交易日");
    await selected.screenshot({ path: join(__dirname, "../test-results/dataset-selected.png") });
    for (const [label, value] of [
      ["交易记录目录", directory],
      ["初始模拟资金", "1000"],
      ["每手保证金", "100"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
      ["单笔数量上限", "1"],
      ["总持仓量上限", "1"],
      ["在途委托数上限", "1"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    // Per-product fee template: saved with its source, then filled back in.
    const template = page.getByRole("region", { name: "费率模板" });
    await template.getByRole("button", { name: "保存为模板", exact: true }).click();
    await template.getByLabel("费率来源", { exact: true }).fill("fixture schedule");
    await template.getByLabel("生效日期", { exact: true }).fill("2026-09-01");
    await template.getByRole("button", { name: "确认保存", exact: true }).click();
    await expect(template).toContainText("来源 fixture schedule · 2026-09-01 起生效");
    await page.getByLabel("每手开仓费", { exact: true }).fill("9");
    await template.getByRole("button", { name: "填入模板", exact: true }).click();
    await expect(page.getByLabel("每手开仓费", { exact: true })).toHaveValue("2");
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await page.getByRole("button", { name: "均线回测", exact: true }).click();
    await expect(page.getByLabel("初始资金", { exact: true })).toHaveValue("");
    await page.getByLabel("初始资金", { exact: true }).fill("9000");
    await page.getByRole("button", { name: "交易", exact: true }).click();
    await expect(page.getByLabel("交易记录目录", { exact: true })).toHaveValue(directory);
    await expect(page.getByLabel("初始模拟资金", { exact: true })).toHaveValue("1000");
    await expect(page.getByLabel("单笔数量上限", { exact: true })).toHaveValue("1");
    await page.getByRole("button", { name: "创建模拟会话", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("1000 CNY");
    await page.getByRole("button", { name: "回放下一根", exact: true }).click();
    await page.getByLabel("限价", { exact: true }).fill("100");
    await page.getByLabel("委托手数", { exact: true }).fill("2");
    await page.getByRole("button", { name: "提交模拟委托", exact: true }).click();
    await expect(page.getByRole("alert")).toBeVisible();
    await expect(page.getByTestId("paper-frozen")).toHaveText("0 CNY");
    await expect(page.getByRole("table", { name: "模拟委托" }).locator("tbody tr")).toHaveCount(0);
    await page.getByLabel("委托手数", { exact: true }).fill("1");

    await page.getByRole("button", { name: "提交模拟委托", exact: true }).click();
    await expect(page.getByTestId("paper-frozen")).toHaveText("102 CNY");
    await expect(page.getByRole("table", { name: "模拟成交" }).locator("tbody tr")).toHaveCount(0);
    await page.getByRole("button", { name: "回放下一根", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("998 CNY");
    await expect(page.getByRole("table", { name: "模拟持仓" })).toContainText("多头");
    await page.getByRole("button", { name: "总览", exact: true }).click();
    const summary = page.getByRole("region", { name: "账户与持仓", exact: true });
    await expect(summary.getByText("历史模拟 · CNY", { exact: true })).toBeVisible();
    await expect(summary.getByRole("region", { name: "模拟持仓", exact: true })).toContainText(
      "rb2610",
    );
    await page.screenshot({ path: join(__dirname, "../test-results/overview-account.png") });
    await summary.getByRole("button", { name: "查看账户", exact: true }).click();
    await page.getByLabel("买卖方向", { exact: true }).selectOption("sell");
    await page.getByLabel("开平仓", { exact: true }).selectOption("close_today");
    await page.getByLabel("限价", { exact: true }).fill("110");
    await page.getByRole("button", { name: "提交模拟委托", exact: true }).click();
    await expect(page.getByTestId("paper-frozen")).toHaveText("3 CNY");
    await page.getByRole("button", { name: "回放下一根", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("1105 CNY");
    await expect(page.getByTestId("paper-fees")).toHaveText("5 CNY");
    await expect(page.getByRole("table", { name: "模拟持仓" }).locator("tbody tr")).toHaveCount(0);
    await expect(page.getByRole("table", { name: "模拟成交" }).locator("tbody tr")).toHaveCount(2);
    await page.screenshot({ path: join(__dirname, "../test-results/paper-trading.png") });
    const response = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const state = await response.json();
    const tradingPid = state.result.diagnostics.trading_process_id as number;
    expect(tradingPid).toBeGreaterThan(0);
    process.kill(tradingPid, "SIGKILL");
    await page.getByRole("button", { name: "查看服务连接", exact: true }).click();
    await page.getByRole("button", { name: "重新检测", exact: true }).click();
    await page.keyboard.press("Escape");
    await expect(
      page.getByText("交易连接或存储状态不确定，请关闭会话并从原目录恢复，核对结果后再操作。"),
    ).toBeVisible();
    await page.getByRole("button", { name: "总览", exact: true }).click();
    await expect(page.getByText("模拟会话需要恢复", { exact: true })).toBeVisible();
    await page.getByRole("button", { name: "交易", exact: true }).click();
    await page.getByRole("button", { name: "断开连接", exact: true }).click();
    await page.reload();
    await page.getByRole("button", { name: "交易", exact: true }).click();
    await page.getByLabel("交易记录目录", { exact: true }).fill(directory);
    await page.getByRole("button", { name: "恢复会话", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("1105 CNY");
    await expect(page.getByRole("table", { name: "模拟成交" }).locator("tbody tr")).toHaveCount(2);
    const restoredResponse = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "runtime.snapshot", params: {} },
    });
    expect((await restoredResponse.json()).result.paper.risk).toEqual({
      max_order_quantity: "1",
      max_gross_quantity: "1",
      max_working_orders: 1,
    });
    await page.getByRole("button", { name: "断开连接", exact: true }).click();
  } finally {
    await page.request.post("/__asterion/api", {
      data: { version: 1, method: "paper.close", params: {} },
    });
    await removeFolder(folder);
  }
});
