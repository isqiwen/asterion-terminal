import { test, expect, type Page } from "./test";

// Board controls live in the market ⋯ menu once quotes are flowing.
async function marketMenu(page: Page) {
  const menu = page.getByRole("dialog", { name: "行情设置" });
  if (!(await menu.count())) await page.getByRole("button", { name: "行情设置" }).click();
  return menu;
}

test("full market loads automatically and watchlist membership stays independent", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const panel = page.getByRole("region", { name: "实时期货行情" });
  await panel.getByLabel("行情前置", { exact: true }).fill("tcp://127.0.0.1:1");
  await panel.getByLabel("目录查询前置", { exact: true }).fill("tcp://127.0.0.1:1");
  await panel.getByLabel("经纪商代码", { exact: true }).fill("test");
  await panel.getByLabel("用户代码", { exact: true }).fill("fixture");
  await panel.getByLabel("密码", { exact: true }).fill("ui-fixture-secret");
  await panel.getByRole("button", { name: "连接行情", exact: true }).click();
  await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  await expect((await marketMenu(page)).getByRole("button", { name: "全部月份" })).toBeVisible();
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(0);
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await (await marketMenu(page)).getByRole("button", { name: "加入自选", exact: true }).click();
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(1);
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await (await marketMenu(page)).getByRole("button", { name: "移出自选", exact: true }).click();
  await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "ui-fixture-secret",
  );
  await page.screenshot({ path: "apps/clients/terminal/test-results/full-market.png" });
  await (await marketMenu(page)).getByRole("button", { name: "断开行情", exact: true }).click();
});

test("login without subscriptions keeps the add-contract workflow visible", async ({ page }) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const panel = page.getByRole("region", { name: "实时期货行情" });
  await panel.getByLabel("行情前置", { exact: true }).fill("tcp://127.0.0.1:1");
  await panel.getByLabel("经纪商代码", { exact: true }).fill("test");
  await panel.getByLabel("用户代码", { exact: true }).fill("fixture");
  await panel.getByLabel("密码", { exact: true }).fill("ui-fixture-secret");
  await panel.getByRole("button", { name: "连接行情", exact: true }).click();
  await expect(
    panel.getByText("已登录，尚未订阅合约。选择交易所并添加实际月份合约后接收行情。", {
      exact: true,
    }),
  ).toBeVisible();
  await panel.getByLabel("实际合约", { exact: true }).fill("rb2610");
  await panel.getByRole("button", { name: "添加自选", exact: true }).click();
  await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  await (await marketMenu(page)).getByRole("button", { name: "断开行情", exact: true }).click();
});

test("read-only market workspace receives C++ test SDK quotes without storing credentials", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const panel = page.getByRole("region", { name: "实时期货行情" });
  await panel.getByLabel("实际合约", { exact: true }).fill("rb2610");
  await panel.getByRole("button", { name: "添加自选" }).click();
  await panel.getByLabel("行情前置", { exact: true }).fill("tcp://127.0.0.1:1");
  await panel.getByLabel("经纪商代码", { exact: true }).fill("test");
  await panel.getByLabel("用户代码", { exact: true }).fill("fixture");
  await panel.getByLabel("密码", { exact: true }).fill("ui-fixture-secret");
  const connect = panel.getByRole("button", { name: "连接行情", exact: true });
  await expect(panel.locator(".market-session > .panel-heading").getByRole("status")).toHaveText(
    "未登录",
  );
  await expect(panel.getByRole("button", { name: "启动本机行情服务" })).toHaveCount(0);
  await expect(connect).toBeEnabled();
  // Polls after connecting carry held market revisions and receive deltas.
  const incremental = page.waitForResponse(async response => {
    const body = response.request().postDataJSON() as { params?: Record<string, unknown> } | null;
    if (!response.url().includes("/__asterion/api") || body?.params?.market_rows === undefined)
      return false;
    const json = (await response.json()) as {
      result?: { unchanged?: boolean; market?: { delta?: boolean } };
    };
    return json.result?.unchanged === true || json.result?.market?.delta === true;
  });
  await connect.click();
  await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  await incremental;
  const detail = panel.getByRole("complementary", { name: "合约详情" });
  await detail.getByRole("button", { name: "Tick", exact: true }).first().click();
  await expect(panel.getByRole("img", { name: "最近报价走势" })).toBeVisible();
  await expect(panel.locator(".contract-live-chart .price-chart-cursor")).toContainText("3510");
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "ui-fixture-secret",
  );
  await expect(panel.getByLabel("密码", { exact: true })).toHaveValue("");
  await page.screenshot({ path: "apps/clients/terminal/test-results/live-market.png" });
  await (await marketMenu(page)).getByRole("button", { name: "断开行情", exact: true }).click();
  await expect(panel.locator(".market-session > .panel-heading").getByRole("status")).toHaveText(
    "未登录",
  );
  await expect(panel.getByRole("img", { name: "最近报价走势" })).toHaveCount(0);
  await expect(detail.locator(".quote-pane-status").first()).toHaveText("断线 · 旧报价");
});
