import { openView } from "./navigation-helper";
import { liveAccount, rpc } from "./rpc-fixture";
import { test, expect } from "./trading-fixture";
import { join } from "node:path";

// Live trading against the independent trading host’s test-only CTP trader SDK: the
// order form stays closed until the account is connected and authorized.
test(
  "live CTP session connects, authorizes and trades through the execution chain",
  { tag: ["@journey", "@trading"] },
  async ({ page, tradingHost }) => {
    // The broker's flow limit spaces queries a second apart; the whole chain
    // plus the backtest form does not fit the default budget.
    test.setTimeout(90000);
    const password = "test-only-password";
    const account = tradingHost.connection;
    await page.goto("/");
    await rpc(page.request, "market.open");
    await page.reload();
    await openView(page, "trading");
    const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
    // Both accounts are listed; each has its own state and panel.
    const list = panel.getByRole("navigation", { name: "CTP 账户" });
    await expect(list.getByRole("button", { name: /^other-account/ })).toHaveCount(0);
    await list.getByRole("button", { name: /^live-account/ }).click();
    await expect(panel.getByRole("status", { name: "CTP 账户" })).toContainText("live-account");
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
    await expect(panel.getByRole("combobox", { name: "柜台环境", exact: true })).toHaveCount(0);
    await expect(panel.getByRole("combobox", { name: "交易服务连接", exact: true })).toHaveCount(0);
    await panel.getByRole("button", { name: "下一步", exact: true }).click();
    await panel.getByRole("button", { name: "创建 CTP 账户", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("已就绪", { timeout: 20000 });
    const capacity = (await liveAccount(page.request, account)).capacity;
    expect(capacity.recordsLimit).toBe("100001");
    expect(Number(capacity.recordsUsed)).toBeGreaterThan(0);
    expect(Number(capacity.bytesUsed)).toBeGreaterThan(0);
    const order = panel.getByRole("form", { name: "CTP 委托" });
    // Only the current step is offered: no order ticket before the counter
    // connection and the permission to send.
    await expect(order).toHaveCount(0);
    await expect(panel.getByRole("combobox", { name: "柜台环境", exact: true })).toHaveCount(0);
    await panel.getByLabel("我已核对账户与柜台环境", { exact: true }).click();
    await expect(panel.getByLabel("交易密码", { exact: true })).toHaveCount(0);
    await expect(panel.getByLabel("授权码", { exact: true })).toHaveCount(0);
    await expect(panel.getByTestId("live-phase")).toHaveText("已就绪", { timeout: 20000 });
    await expect(order).toHaveCount(0);
    await expect(
      page.locator(".status-bar").getByRole("button", { name: "● 交易·live-account", exact: true }),
    ).toBeVisible();
    // The broker reports the account's own rates.
    await panel.getByRole("button", { name: "费率", exact: true }).click();
    await panel.getByRole("button", { name: "查询账户费率", exact: true }).click();
    const rates = panel.getByRole("table", { name: "账户费率表" });
    await expect(rates.locator("tbody tr")).toContainText("已返回", { timeout: 15000 });
    await expect(rates.locator("tbody tr")).toContainText("0 / 0.12");
    const authorize = panel.getByRole("button", { name: "允许发送委托", exact: true });
    await expect(authorize).toBeDisabled();
    await panel.screenshot({ path: join(__dirname, "../test-results/live-authorize.png") });
    await panel.getByLabel("我确认使用账户 000001 向上方柜台发送委托", { exact: true }).check();
    await authorize.click();
    await expect(panel.getByText("已授权", { exact: true })).toBeVisible();

    await order.getByLabel("委托手数", { exact: true }).fill("1");
    await order.getByLabel("限价", { exact: true }).fill("3500.3");
    await order.getByRole("button", { name: "买入 live-account", exact: true }).click();
    // A refused order says why in the account panel, where it was sent from.
    const refused = panel.locator(".action-error");
    await expect(refused).toBeVisible();
    await expect(
      panel.getByRole("table", { includeHidden: true, name: "CTP 委托记录" }).locator("tbody tr"),
    ).toHaveCount(0);
    await order.getByLabel("限价", { exact: true }).fill("3600");
    await order.getByRole("button", { name: "买入 live-account", exact: true }).click();
    await expect(refused).toContainText("限价偏离最新价超过本会话的上限，委托未发送。");
    await order.getByLabel("限价", { exact: true }).fill("3500.5");
    await order.getByRole("button", { name: "买入 live-account", exact: true }).click();
    await expect(
      panel.getByRole("table", { includeHidden: true, name: "CTP 成交" }).locator("tbody tr"),
    ).toHaveCount(1, {
      timeout: 10000,
    });
    await expect(panel.getByRole("table", { includeHidden: true, name: "CTP 持仓" })).toContainText(
      "rb2610",
    );
    const filled = await liveAccount(page.request, account);
    expect(filled.orders.some((order: { status: string }) => order.status === "filled")).toBe(true);
    await expect(
      page
        .locator(".status-bar")
        .getByRole("button", { name: "● 交易·live-account 已授权", exact: true }),
    ).toBeVisible();
    await openView(page, "research");
    await page
      .locator(".status-bar")
      .getByRole("button", { name: "● 交易·live-account 已授权", exact: true })
      .click();
    await expect(panel.getByTestId("live-phase")).toHaveText("已就绪");

    await panel.getByRole("button", { name: "委托", exact: true }).click();
    await expect(panel.getByRole("table", { name: "CTP 委托记录" })).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/live-trading.png") });
    const state = await rpc(page.request, "runtime.snapshot");
    expect(JSON.stringify(state)).not.toContain(password);

    await panel.getByRole("button", { name: "撤销授权", exact: true }).click();
    await expect(order).toHaveCount(0);
    await panel.getByRole("button", { name: "断开账户", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("未连接");

    // A reopened Terminal reads the account again without logging in.
    await page.reload();
    await expect(list.getByRole("button", { name: /^live-account/ })).toContainText("未连接", {
      timeout: 30000,
    });
    await expect(panel.getByTestId("live-phase")).toHaveText("未连接");

    // A restarted test SDK has no broker-side order memory. Durable terminal
    // evidence keeps the filled intent retired; recovery never resends it.
    await tradingHost.restart();
    await expect
      .poll(async () => (await liveAccount(page.request, account)).phase, { timeout: 20000 })
      .toBe("ready");
    const recovered = await liveAccount(page.request, account);
    expect(recovered.unconfirmed).toBeUndefined();
    expect(recovered.orders).toBeUndefined();
    expect(recovered.authorization).toBeUndefined();
  },
);
