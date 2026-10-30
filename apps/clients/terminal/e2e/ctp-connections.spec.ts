import { openView } from "./navigation-helper";
import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";

test("several CTP market accounts are kept while exactly one supplies market data", async ({
  page,
}) => {
  await page.goto("/");
  const settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "数据源", exact: true }).click();
  const region = settings.getByRole("region", { name: "CTP 行情账户", exact: true });
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
  const marketBadge = ".settings-badge.accent";
  await expect(first.locator(marketBadge)).toHaveText("用于行情");

  await openView(page, "market");
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const market = page.getByRole("region", { name: "实时期货行情" });
  await expect(market.getByRole("status", { name: "行情来源" })).toContainText(
    "仿真柜台（9999 · 000001）",
  );
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
  // Saving more did not change where market data comes from.
  await expect(first.locator(marketBadge)).toBeVisible();
  const second = region.getByRole("region", { name: "实盘柜台甲", exact: true });
  await expect(second.locator(marketBadge)).toHaveCount(0);
  await second.getByRole("button", { name: "用于行情", exact: true }).click();
  await expect(second.locator(marketBadge)).toBeVisible();
  await expect(first.locator(marketBadge)).toHaveCount(0);
  await settings.screenshot({ path: "apps/clients/terminal/test-results/ctp-accounts.png" });

  // Removing the market data account leaves none chosen rather than picking another.
  await second.getByRole("button", { name: "删除账户", exact: true }).click();
  await second.getByRole("button", { name: "确认删除账户", exact: true }).click();
  await expect(second).toHaveCount(0);
  await expect(region.locator(marketBadge)).toHaveCount(0);
  await expect(region.getByRole("region", { name: "实盘柜台乙", exact: true })).toBeVisible();
});
