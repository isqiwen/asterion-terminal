import { rpc, seedDataset } from "./dataset-fixture";
import { removeFolder } from "./cleanup";
import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

// Live trading against the isolated Agent's test-only CTP trader SDK: the
// order form stays closed until the account is connected and authorized.
test("live CTP session connects, authorizes and trades through the execution chain", async ({
  page,
}) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-live-e2e-"));
  const directory = join(folder, "account");
  await mkdir(directory);
  const password = "e2e-live-password";
  try {
    await page.goto("/");
    await rpc(page.request, "market.local");
    await rpc(page.request, "market.catalog", {
      front: "tcp://127.0.0.1:1",
      broker: "test",
      user: "catalog",
      password: "catalog-only",
      app_id: "",
      auth_code: "",
    });
    await expect
      .poll(async () => (await rpc(page.request, "runtime.snapshot")).market.catalog.phase)
      .toBe("ready");
    await page.reload();
    await page.getByRole("button", { name: "交易", exact: true }).click();
    await page.getByRole("button", { name: "实盘", exact: true }).click();
    const panel = page.getByRole("region", { name: "期货实盘交易", exact: true });
    await expect(
      panel.getByText("实盘会连接真实期货账户并发送真实委托", { exact: false }),
    ).toBeVisible();
    await panel.getByLabel("实盘记录目录", { exact: true }).fill(directory);
    for (const [label, value] of [
      ["交易前置地址", "tcp://127.0.0.1:41205"],
      ["经纪商代码", "9999"],
      ["投资者账号", "000001"],
      ["AppID", "client_app"],
      ["单笔数量上限", "5"],
      ["总持仓量上限", "10"],
      ["在途委托数上限", "2"],
      ["价格偏离上限", "0.02"],
    ])
      await panel.getByLabel(label, { exact: true }).fill(value);
    await panel.getByLabel("添加合约", { exact: true }).fill("SHFE.rb2610");
    await panel.getByRole("button", { name: "添加", exact: true }).click();
    await panel.getByRole("button", { name: "创建实盘会话", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("未连接");
    const order = panel.getByRole("form", { name: "实盘委托" });
    await expect(order.getByRole("button", { name: "提交实盘委托" })).toBeDisabled();

    await panel.getByLabel("交易密码", { exact: true }).fill(password);
    await panel.getByLabel("授权码", { exact: true }).fill("auth-code");
    await panel.getByRole("button", { name: "连接账户", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("已就绪", { timeout: 20000 });
    await expect(order.getByRole("button", { name: "提交实盘委托" })).toBeDisabled();
    // The account's own rates become the product's fee template.
    await panel.getByRole("button", { name: "查询账户费率", exact: true }).click();
    const rates = panel.getByRole("table", { name: "账户费率表" });
    await expect(rates.locator("tbody tr")).toContainText("已返回", { timeout: 15000 });
    await expect(rates.locator("tbody tr")).toContainText("0 / 0.12");
    await rates.getByRole("button", { name: "存为 SHFE/rb 费率模板", exact: true }).click();
    await expect(rates.getByText("已存为 SHFE/rb 模板", { exact: true })).toBeVisible();
    const authorize = panel.getByRole("button", { name: "授权实盘交易", exact: true });
    await expect(authorize).toBeDisabled();
    await panel.screenshot({ path: join(__dirname, "../test-results/live-authorize.png") });
    await panel.getByLabel("我确认使用账户 000001 发送真实委托", { exact: true }).check();
    await authorize.click();
    await expect(panel.getByText("已授权 · 交易日 20260928", { exact: true })).toBeVisible();

    await order.getByLabel("委托手数", { exact: true }).fill("1");
    await order.getByLabel("限价", { exact: true }).fill("3500.3");
    await order.getByRole("button", { name: "提交实盘委托" }).click();
    await expect(panel.getByRole("alert")).toBeVisible();
    await expect(
      panel.getByRole("table", { name: "实盘委托记录" }).locator("tbody tr"),
    ).toHaveCount(0);
    await order.getByLabel("限价", { exact: true }).fill("3600");
    await order.getByRole("button", { name: "提交实盘委托" }).click();
    await expect(panel.getByRole("alert")).toContainText(
      "限价偏离最新价超过本会话的上限，委托未发送。",
    );
    await order.getByLabel("限价", { exact: true }).fill("3500.5");
    await order.getByRole("button", { name: "提交实盘委托" }).click();
    await expect(panel.getByRole("table", { name: "实盘成交" }).locator("tbody tr")).toHaveCount(
      1,
      {
        timeout: 10000,
      },
    );
    await expect(panel.getByRole("table", { name: "实盘持仓" })).toContainText("rb2610");
    await page.screenshot({ path: join(__dirname, "../test-results/live-trading.png") });
    const state = await rpc(page.request, "runtime.snapshot");
    expect(JSON.stringify(state)).not.toContain(password);

    await panel.getByRole("button", { name: "撤销授权", exact: true }).click();
    await expect(order.getByRole("button", { name: "提交实盘委托" })).toBeDisabled();
    await panel.getByRole("button", { name: "断开账户", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("未连接");

    // Current broker rates must not silently apply to earlier history.
    await seedDataset(page.request, [3500, 3501], "live-rates");
    await page.getByRole("button", { name: "模拟", exact: true }).click();
    const costs = page.getByRole("region", { name: "SHFE · rb2610 保证金与手续费" });
    await expect(costs).toContainText("来源 CTP 账户 9999/000001 · 2026-09-28 起生效");
    await expect(costs.getByRole("button", { name: "填入模板", exact: true })).toBeDisabled();
    await expect(costs.getByRole("alert")).toContainText("费率版本未覆盖首个交易日 2026-09-25");
    await seedDataset(page.request, [3500, 3501], "live-rates-covered", { day: "2026-09-28" });
    await costs.getByRole("button", { name: "填入模板", exact: true }).click();
    await expect(costs.getByLabel("保证金率", { exact: true })).toHaveValue("0.12");
    await expect(costs.getByLabel("平今费率", { exact: true })).toHaveValue("0.0003");
    await expect(costs.getByLabel("每手平今费", { exact: true })).toHaveValue("1.5");
  } finally {
    await page.request.post("/__asterion/api", {
      data: { version: 1, method: "live.close", params: {} },
    });
    await removeFolder(folder);
  }
});
