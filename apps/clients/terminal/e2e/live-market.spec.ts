import { ctpConnection, rpc } from "./dataset-fixture";
import { checkSnapshot } from "./snapshot-schema";
import { test, expect, type Page } from "./test";

// The market data account of these specs.
async function account(page: Page, user = "fixture") {
  await ctpConnection(page.request, "market-account", {
    broker_id: "test",
    user_id: user,
    market: true,
  });
}

// Board controls live in the market ⋯ menu once quotes are flowing.
async function marketMenu(page: Page) {
  const menu = page.getByRole("dialog", { name: "行情设置" });
  if (!(await menu.count())) await page.getByRole("button", { name: "行情设置" }).click();
  return menu;
}

test("full market loads automatically and watchlist membership stays independent", async ({
  page,
}) => {
  // This case measures membership, while the next case and native process
  // tests exercise reconnects. A forced reconnect can disable a button between
  // pointer-down and click and correctly prevent the subscription command.
  await account(page, "steady");
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const panel = page.getByRole("region", { name: "实时期货行情" });
  await expect(panel.getByRole("status", { name: "行情来源" })).toContainText("market-account");
  await panel.getByLabel("密码", { exact: true }).fill("ui-fixture-secret");
  await panel.getByLabel("授权码", { exact: true }).fill("ui-fixture-auth");
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

test("read-only market workspace receives C++ test SDK quotes without storing credentials", async ({
  page,
}) => {
  await account(page);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  const panel = page.getByRole("region", { name: "实时期货行情" });
  await panel.getByLabel("实际合约", { exact: true }).fill("rb2610");
  await panel.getByRole("button", { name: "添加自选" }).click();
  await expect(panel.getByRole("status", { name: "行情来源" })).toContainText("market-account");
  await panel.getByLabel("密码", { exact: true }).fill("ui-fixture-secret");
  await panel.getByLabel("授权码", { exact: true }).fill("ui-fixture-auth");
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
  // Quote arrival can precede catalog completion. Only a settled catalog is
  // unchanged between the full read and the incremental read below.
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).market?.catalog.phase)
    .toBe("ready");
  const held: unknown = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("full market quote snapshot", held);
  const market = held.market!;
  const delta: unknown = await rpc(page.request, "runtime.snapshot", {
    since: 0,
    market_rows: Math.max(0, ...market.subscriptions.map(row => row.revision ?? 0)),
    market_set: market.subscription_set,
    catalog: market.catalog.revision,
  });
  checkSnapshot("market delta with omitted unchanged catalog", delta);
  expect(delta.market?.delta).toBe(true);
  expect(delta.market?.catalog.omitted).toBe(true);
  const detail = panel.getByRole("complementary", { name: "合约详情" });
  await detail.getByRole("button", { name: "Tick", exact: true }).first().click();
  await expect(panel.getByRole("img", { name: "最近报价走势" })).toBeVisible();
  await expect(panel.locator(".contract-live-chart .price-chart-cursor")).toContainText("3510");
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "ui-fixture-secret",
  );
  // Once connected, the connection controls move into the market settings portal.
  await expect((await marketMenu(page)).getByLabel("密码", { exact: true })).toHaveValue("");
  await page.getByRole("button", { name: "行情设置" }).click();
  await page.screenshot({ path: "apps/clients/terminal/test-results/live-market.png" });
  await (await marketMenu(page)).getByRole("button", { name: "断开行情", exact: true }).click();
  await expect(panel.locator(".market-session > .panel-heading").getByRole("status")).toHaveText(
    "未登录",
  );
  await expect(panel.getByRole("img", { name: "最近报价走势" })).toHaveCount(0);
  await expect(detail.locator(".quote-pane-status").first()).toHaveText("断线 · 旧报价");
});

test("remembered market login is reused with blank fields and can be cleared", async ({ page }) => {
  await rpc(page.request, "market.disconnect");
  await account(page, "steady");
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  const panel = page.getByRole("region", { name: "实时期货行情" });
  await panel.getByLabel("密码", { exact: true }).fill("remember-fixture");
  await panel.getByLabel("授权码", { exact: true }).fill("fixture-auth");
  await panel.getByLabel("保存密码和授权码到本机钥匙串").check();
  await panel.getByRole("button", { name: "连接行情", exact: true }).click();
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).market?.phase)
    .toBe("connected");
  await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  await (await marketMenu(page)).getByRole("button", { name: "断开行情", exact: true }).click();
  await page.reload();
  await expect(panel.getByLabel("密码", { exact: true })).toHaveValue("");
  await expect(panel.getByLabel("授权码", { exact: true })).toHaveValue("");
  await panel.getByRole("button", { name: "连接行情", exact: true }).click();
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).market?.phase)
    .toBe("connected");
  await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
  const stored = await page.evaluate(() => JSON.stringify(localStorage));
  expect(stored).not.toContain("remember-fixture");
  expect(stored).not.toContain("fixture-auth");
  const settings = await marketMenu(page);
  await settings.getByText("CTP 连接与自选", { exact: true }).click();
  await settings.getByRole("button", { name: "清除已保存的登录凭据" }).click();
  await (await marketMenu(page)).getByRole("button", { name: "断开行情", exact: true }).click();
  await page.reload();
  await panel.getByRole("button", { name: "连接行情", exact: true }).click();
  await expect(panel.getByRole("alert")).toContainText("没有可用的已保存行情凭据");
});
