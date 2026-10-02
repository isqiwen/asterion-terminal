import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";

test("several CTP accounts trade side by side while one supplies market data", async ({ page }) => {
  await page.goto("/");
  const settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "CTP 账户", exact: true }).click();
  const region = settings.getByRole("region", { name: "CTP 账户", exact: true });
  const fill = async (fields: Record<string, string>) => {
    for (const [label, value] of Object.entries(fields))
      await region.getByLabel(label, { exact: true }).fill(value);
  };
  // The form stays collapsed until an account is added.
  await expect(region).toContainText("还没有 CTP 账户");
  await region.getByRole("button", { name: "添加账户", exact: true }).click();
  await fill({
    账户名称: "仿真柜台",
    经纪商代码: "9999",
    投资者账号: "000001",
    AppID: "client_app",
    交易前置: "tcp://127.0.0.1:41205",
    行情前置: "127.0.0.1:41213",
  });
  await region.getByRole("button", { name: "保存账户", exact: true }).click();
  await expect(region.getByRole("alert")).toContainText("行情前置格式应为 tcp://主机:端口");
  await fill({ 行情前置: "tcp://127.0.0.1:41213" });
  await region.getByRole("button", { name: "保存账户", exact: true }).click();
  // The first account supplies market data without a further step.
  const first = region.getByRole("region", { name: "仿真柜台", exact: true });
  const marketBadge = ".ctp-badge.market";
  await expect(first.locator(marketBadge)).toHaveText("用于行情");

  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const market = page.getByRole("region", { name: "实时期货行情" });
  await expect(market.getByRole("status", { name: "行情来源" })).toContainText(
    "仿真柜台（9999 · 000001）",
  );
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  const trading = page.getByRole("region", { name: "CTP 交易账户", exact: true });

  for (const [name, user] of [
    ["实盘柜台甲", "000002"],
    ["实盘柜台乙", "000003"],
  ]) {
    await region.getByRole("button", { name: "添加账户", exact: true }).click();
    await fill({
      账户名称: name,
      经纪商代码: "8888",
      投资者账号: user,
      AppID: "client_app",
      交易前置: "tcp://127.0.0.1:41205",
      行情前置: "tcp://127.0.0.1:41213",
    });
    await region.getByRole("button", { name: "保存账户", exact: true }).click();
    await expect(region.getByRole("region", { name, exact: true })).toBeVisible();
  }
  // Every account can trade; saving more did not change where market data comes from.
  const list = trading.getByRole("navigation", { name: "CTP 账户" });
  await expect(list.getByRole("button", { name: /^实盘柜台/ })).toHaveCount(2);
  await expect(list.getByRole("button", { name: /^仿真柜台/ })).toHaveCount(1);
  await expect(first.locator(marketBadge)).toBeVisible();
  await list.getByRole("button", { name: /^实盘柜台乙/ }).click();
  await expect(trading.getByRole("status", { name: "CTP 账户" })).toContainText(
    "实盘柜台乙 · 8888 · 000003 · tcp://127.0.0.1:41205",
  );
  const second = region.getByRole("region", { name: "实盘柜台甲", exact: true });
  await expect(second.locator(marketBadge)).toHaveCount(0);
  await second.getByRole("button", { name: "用于行情", exact: true }).click();
  await expect(second.locator(marketBadge)).toBeVisible();
  await expect(first.locator(marketBadge)).toHaveCount(0);
  await settings.screenshot({ path: "apps/clients/terminal/test-results/ctp-accounts.png" });
  await page.screenshot({ path: "apps/clients/terminal/test-results/ctp-trading-accounts.png" });

  // Removing the market data account leaves none chosen rather than picking another.
  await second.getByRole("button", { name: "删除账户", exact: true }).click();
  await second.getByRole("button", { name: "确认删除账户", exact: true }).click();
  await expect(second).toHaveCount(0);
  await expect(region.locator(marketBadge)).toHaveCount(0);
  await expect(list.getByRole("button", { name: /^实盘柜台/ })).toHaveCount(1);
});
