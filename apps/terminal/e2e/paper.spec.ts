import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("paper trading uses C++ ledger and restores persisted account", async ({ page }) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-paper-e2e-"));
  const csv = join(folder, "ticks.csv"),
    directory = join(folder, "account");
  await mkdir(directory);
  await writeFile(
    csv,
    "timestamp_ns,price,quantity\n1790384400000000000,100,1\n1790384401000000000,99,1\n1790384402000000000,110,1\n",
  );
  try {
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible();
    await page.getByRole("button", { name: "数据", exact: true }).click();
    for (const [label, value] of [
      ["CSV 文件路径", csv],
      ["品种代码", "rb"],
      ["实际合约", "rb2610"],
      ["交割月份", "2026-10"],
      ["价格步长", "1"],
      ["每手乘数", "10"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "校验并预览", exact: true }).click();
    await expect(page.getByText("文件校验完成，可在市场工作区查看历史成交。")).toBeVisible();
    await page.getByRole("button", { name: "交易", exact: true }).click();
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
    await page.getByRole("button", { name: "创建模拟会话", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("1000 CNY");
    await page.getByRole("button", { name: "回放下一笔", exact: true }).click();
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
    await page.getByRole("button", { name: "回放下一笔", exact: true }).click();
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
    await page.getByRole("button", { name: "回放下一笔", exact: true }).click();
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
    await rm(folder, { recursive: true, force: true });
  }
});
