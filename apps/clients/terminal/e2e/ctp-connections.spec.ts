import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";

test("several CTP accounts are stored and the current one drives market data and trading", async ({
  page,
}) => {
  await page.goto("/");
  const settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "CTP 账户", exact: true }).click();
  const region = settings.getByRole("region", { name: "CTP 账户", exact: true });
  const fill = async (fields: Record<string, string>) => {
    for (const [label, value] of Object.entries(fields))
      await region.getByLabel(label, { exact: true }).fill(value);
  };
  await fill({ 账户名称: "仿真柜台", 经纪商代码: "9999", 投资者账号: "000001" });
  // Neither front is filled yet: nothing to connect to, so nothing to save.
  await expect(region.getByRole("button", { name: "保存账户", exact: true })).toBeDisabled();
  await fill({ 行情前置: "127.0.0.1:41213" });
  await region.getByRole("button", { name: "保存账户", exact: true }).click();
  await expect(region.getByRole("alert")).toContainText("CTP 账户设置无效");
  await fill({ 行情前置: "tcp://127.0.0.1:41213" });
  await region.getByRole("button", { name: "保存账户", exact: true }).click();
  // The first account becomes the current one without a further step.
  const first = region.getByRole("region", { name: "仿真柜台", exact: true });
  await expect(first.getByRole("heading")).toContainText("当前账户");
  await expect(first).toContainText("交易前置 未填写");

  // Market data only: usable for the market login, not yet for trading.
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const market = page.getByRole("region", { name: "实时期货行情" });
  const marketAccount = market.getByRole("status", { name: "当前 CTP 账户" });
  await expect(marketAccount).toContainText("仿真柜台（9999 · 000001）");
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  const trading = page.getByRole("region", { name: "CTP 交易账户", exact: true });
  await trading.getByRole("button", { name: "添加 CTP 账户", exact: true }).click();
  const tradingAccount = trading.getByRole("status", { name: "当前 CTP 账户" });
  await expect(tradingAccount).toContainText("未填写交易前置");
  await expect(trading.getByRole("button", { name: "下一步", exact: true })).toBeDisabled();

  await fill({
    账户名称: "实盘柜台",
    经纪商代码: "8888",
    投资者账号: "000002",
    AppID: "client_app",
    交易前置: "tcp://127.0.0.1:41205",
    行情前置: "tcp://127.0.0.1:41213",
  });
  await region.getByRole("button", { name: "保存账户", exact: true }).click();
  const second = region.getByRole("region", { name: "实盘柜台", exact: true });
  // Saving another account does not change which one is in use.
  await expect(second.getByRole("heading")).not.toContainText("当前账户");
  await expect(tradingAccount).toContainText("仿真柜台");
  await second.getByRole("button", { name: "设为当前账户", exact: true }).click();
  await expect(second.getByRole("heading")).toContainText("当前账户");
  await expect(first.getByRole("heading")).not.toContainText("当前账户");
  await expect(tradingAccount).toContainText("实盘柜台 · 8888 · 000002 · tcp://127.0.0.1:41205");
  await expect(trading.getByRole("button", { name: "下一步", exact: true })).toBeEnabled();
  await settings.screenshot({ path: "apps/clients/terminal/test-results/ctp-accounts.png" });

  // Removing the current account leaves none selected rather than picking another.
  await second.getByRole("button", { name: "删除账户", exact: true }).click();
  await second.getByRole("button", { name: "确认删除账户", exact: true }).click();
  await expect(second).toHaveCount(0);
  await expect(first.getByRole("heading")).not.toContainText("当前账户");
  await expect(tradingAccount).toContainText("尚未设置 CTP 账户");
});
