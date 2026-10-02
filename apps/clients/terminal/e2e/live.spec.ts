import { ctpConnection, rpc, seedDataset } from "./dataset-fixture";
import { test, expect } from "./test";
import { join } from "node:path";

// Live trading against the isolated Agent's test-only CTP trader SDK: the
// order form stays closed until the account is connected and authorized.
test("live CTP session connects, authorizes and trades through the execution chain", async ({
  page,
}) => {
  // The broker's flow limit spaces queries a second apart; the whole chain
  // plus the backtest form does not fit the default budget.
  test.setTimeout(90000);
  const password = "e2e-live-password";
  try {
    await ctpConnection(page.request, "live-account", {
      broker_id: "9999",
      user_id: "000001",
      trade_front: "tcp://127.0.0.1:41205",
    });
    await ctpConnection(page.request, "other-account", {
      broker_id: "9999",
      user_id: "000002",
      trade_front: "tcp://127.0.0.1:41205",
    });
    await page.goto("/");
    await rpc(page.request, "market.local");
    await page.reload();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "交易", exact: true })
      .click();
    const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
    // Both accounts are listed; each has its own state and panel.
    const list = panel.getByRole("navigation", { name: "CTP 账户" });
    await expect(list.getByRole("button", { name: /^other-account/ })).toContainText("未开通交易");
    await list.getByRole("button", { name: /^live-account/ }).click();
    await expect(panel.getByRole("status", { name: "CTP 账户" })).toContainText("live-account");
    await panel.getByRole("button", { name: "下一步", exact: true }).click();
    if (await panel.getByLabel("目录查询密码", { exact: true }).isVisible()) {
      await panel.getByLabel("目录查询密码", { exact: true }).fill("catalog-only");
      await panel.getByLabel("目录查询授权码", { exact: true }).fill("auth-code");
      await panel.getByRole("button", { name: "获取合约列表", exact: true }).click();
    }
    await expect(panel.getByLabel("添加合约", { exact: true })).toBeVisible();

    for (const [label, value] of [
      ["单笔数量上限", "5"],
      ["总持仓量上限", "10"],
      ["在途委托数上限", "2"],
      ["价格偏离上限", "0.02"],
    ])
      await panel.getByLabel(label, { exact: true }).fill(value);
    await panel.getByLabel("添加合约", { exact: true }).fill("SHFE.rb2610");
    await panel.getByRole("button", { name: "添加", exact: true }).click();
    await panel.getByRole("button", { name: "下一步", exact: true }).click();
    await panel.getByRole("button", { name: "创建 CTP 账户", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("未连接");
    const order = panel.getByRole("form", { name: "CTP 委托" });
    await expect(order.getByRole("button", { name: "向 live-account 提交委托" })).toBeDisabled();

    await expect(panel.getByRole("button", { name: "连接账户", exact: true })).toBeDisabled();
    await panel.getByLabel("柜台环境", { exact: true }).selectOption("simulation");
    await panel.getByLabel("我已核对账户与柜台环境", { exact: true }).check();
    await panel.getByLabel("交易密码", { exact: true }).fill(password);
    await panel.getByLabel("授权码", { exact: true }).fill("auth-code");
    await panel.getByRole("button", { name: "连接账户", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("已就绪", { timeout: 20000 });
    await expect(order.getByRole("button", { name: "向 live-account 提交委托" })).toBeDisabled();
    await expect(
      page.getByRole("button", { name: "CTP：0 个已授权 · 1 个只读 · 0 个未就绪", exact: true }),
    ).toBeVisible();
    // The account's own rates become the product's fee template.
    await panel.getByText("账户费率与模板", { exact: true }).click();
    await panel.getByRole("button", { name: "查询账户费率", exact: true }).click();
    const rates = panel.getByRole("table", { name: "账户费率表" });
    await expect(rates.locator("tbody tr")).toContainText("已返回", { timeout: 15000 });
    await expect(rates.locator("tbody tr")).toContainText("0 / 0.12");
    await rates.getByRole("button", { name: "存为 SHFE/rb 费率模板", exact: true }).click();
    await expect(rates.getByText("已存为 SHFE/rb 模板", { exact: true })).toBeVisible();
    const authorize = panel.getByRole("button", { name: "允许发送委托", exact: true });
    await expect(authorize).toBeDisabled();
    await panel.screenshot({ path: join(__dirname, "../test-results/live-authorize.png") });
    await panel.getByLabel("我确认使用账户 000001 向上方柜台发送委托", { exact: true }).check();
    await authorize.click();
    await expect(panel.getByText("已授权 · 交易日 20260928", { exact: true })).toBeVisible();

    await order.getByLabel("委托手数", { exact: true }).fill("1");
    await order.getByLabel("限价", { exact: true }).fill("3500.3");
    await order.getByRole("button", { name: "向 live-account 提交委托" }).click();
    await expect(panel.getByRole("alert")).toBeVisible();
    await expect(
      panel.getByRole("table", { includeHidden: true, name: "CTP 委托记录" }).locator("tbody tr"),
    ).toHaveCount(0);
    await order.getByLabel("限价", { exact: true }).fill("3600");
    await order.getByRole("button", { name: "向 live-account 提交委托" }).click();
    await expect(panel.getByRole("alert")).toContainText(
      "限价偏离最新价超过本会话的上限，委托未发送。",
    );
    await order.getByLabel("限价", { exact: true }).fill("3500.5");
    await order.getByRole("button", { name: "向 live-account 提交委托" }).click();
    await expect(
      panel.getByRole("table", { includeHidden: true, name: "CTP 成交" }).locator("tbody tr"),
    ).toHaveCount(1, {
      timeout: 10000,
    });
    await expect(panel.getByRole("table", { includeHidden: true, name: "CTP 持仓" })).toContainText(
      "rb2610",
    );
    await expect(
      page.getByRole("button", { name: "CTP：1 个已授权 · 0 个只读 · 0 个未就绪", exact: true }),
    ).toBeVisible();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page
      .getByRole("button", { name: "CTP：1 个已授权 · 0 个只读 · 0 个未就绪", exact: true })
      .click();
    await expect(panel.getByTestId("live-phase")).toHaveText("已就绪");

    await panel.getByRole("button", { name: "委托", exact: true }).click();
    await expect(panel.getByRole("table", { name: "CTP 委托记录" })).toBeVisible();
    await panel.getByText("账户费率与模板", { exact: true }).click();
    await page.screenshot({ path: join(__dirname, "../test-results/live-trading.png") });
    const state = await rpc(page.request, "runtime.snapshot");
    expect(JSON.stringify(state)).not.toContain(password);

    await panel.getByRole("button", { name: "撤销授权", exact: true }).click();
    await expect(order.getByRole("button", { name: "向 live-account 提交委托" })).toBeDisabled();
    await panel.getByRole("button", { name: "断开账户", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("未连接");

    // Choosing another account only changes the view; this one stays open.
    await list.getByRole("button", { name: /^other-account/ }).click();
    await expect(panel.getByRole("heading", { name: "开通交易：other-account" })).toBeVisible();
    await expect(list.getByRole("button", { name: /^live-account/ })).toContainText("未连接");
    await list.getByRole("button", { name: /^live-account/ }).click();
    await panel.getByRole("button", { name: "关闭账户", exact: true }).click();
    await expect(panel.getByRole("button", { name: "打开账户", exact: true })).toBeVisible();
    await expect(list.getByRole("button", { name: /^live-account/ })).toContainText("未打开");

    // Current broker rates must not silently apply to earlier history.
    await seedDataset(page.request, [3500, 3501], "live-rates");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: "均线回测", exact: true }).click();
    const openCosts = async () => {
      await page.getByRole("button", { name: "新建回测", exact: true }).click();
      await page.getByRole("button", { name: "下一步", exact: true }).click();
    };
    await openCosts();
    const costs = page.getByRole("region", { name: "SHFE · rb2610 保证金与手续费" });
    await expect(costs).toContainText("来源 CTP 账户 9999/000001 · 2026-09-28 起生效");
    await expect(costs.getByRole("button", { name: "填入模板", exact: true })).toBeDisabled();
    await expect(costs.getByRole("alert")).toContainText("费率版本未覆盖首个交易日 2026-09-25");
    await seedDataset(page.request, [3500, 3501], "live-rates-covered", { day: "2026-09-28" });
    // Seeding restarts the research service, which closes the open form.
    await page.reload();
    await page.getByRole("button", { name: "均线回测", exact: true }).click();
    await openCosts();
    await costs.getByRole("button", { name: "填入模板", exact: true }).click();
    await expect(costs.getByLabel("保证金率", { exact: true })).toHaveValue("0.12");
    await expect(costs.getByLabel("平今费率", { exact: true })).toHaveValue("0.0003");
    await expect(costs.getByLabel("每手平今费", { exact: true })).toHaveValue("1.5");
  } finally {
    await page.request.post("/__asterion/api", {
      data: { version: 1, method: "live.close", params: { account: "live-account" } },
    });
  }
});
