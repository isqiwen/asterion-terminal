import { expect, test, type BrowserContext, type Page } from "@playwright/test";
import { nativeContext } from "./native";

async function dashboard(context: BrowserContext, page: Page, empty = false) {
  await nativeContext(context);
  let failed = false;
  const writes: string[] = [];
  const user = { email: "dashboard@example.com", first_name: "Dash", last_name: "Test" };
  const snapshots = ["AU2506", "CU2506"].map((contract, index) => ({
    id: `fixed-${contract}`, job_id: `job-${index}`, manifest: {
      demo: false, frequency: "1d", schema_version: 1, rows: 2, checksum: "fixture",
      contracts: [contract], start: "2025-04-10", end: "2025-04-11", uri: "fixture",
      source: "验收来源", state: "PUBLISHED",
    },
  }));
  await context.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/market/state")) return route.fulfill({json:{environment:"simnow",state:"disconnected",detail:"行情源未连接",configuration:{version:1,front:"",user_id:"",subscriptions:[]},quotes:[],subscription_errors:{},observed_at:Date.now()/1000}});
    if (path.includes("/account/security")) return route.fallback();
    if (route.request().method() === "POST" && !/login|access\/scopes/.test(path)) writes.push(path);
    if (path.endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });
    if (path.endsWith("/snapshots")) return failed ? route.fulfill({ status: 503, json: { detail: "目录暂时不可用" } }) : route.fulfill({ json: empty ? [] : snapshots });
    if (path.endsWith("/bars")) {
      const contract = path.includes("CU2506") ? "CU2506" : "AU2506";
      return route.fulfill({ json: [10, 11].map((day) => ({ contract, event_time: `2025-04-${day}T15:00:00+08:00`, available_at: `2025-04-${day}T15:00:00+08:00`, trading_day: `2025-04-${day}`, open: "100", high: "110", low: "90", close: "105", volume: 100 })) });
    }
    if (path.endsWith("/jobs")) return route.fulfill({ json: empty ? [] : [
      { id: "failed-task", kind: "contract_roles.continue", command_id: "fixture", state: "FAILED", attempt: 1, created_at: 1744358400, error: "固定来源证据不可用", result: null },
      { id: "done-task", kind: "contract_roles.continue", command_id: "fixture-done", state: "SUCCEEDED", attempt: 1, created_at: 1744358300, error: null, result: { computed_version_id: "fixed-role-version" } },
    ] });
    if (path.endsWith("/data/catalog")) return route.fulfill({ json: { items: [], total: 0 } });
    return route.fulfill({ json: path.endsWith("/login") ? { session: "session", user } : path.endsWith("/me") ? user : path.endsWith("/health") ? { status: "ready" } : [] });
  });
  await page.goto("/?view=总览");
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page.getByLabel("密码", { exact: true }).fill("fixture-password-123");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await expect(page.getByLabel("工作总览", { exact: true })).toBeVisible();
  return { fail: () => { failed = true; }, writes };
}

test("overview is realtime-only even with historical data; object controls belong to market", async ({context,page}) => {
  const state = await dashboard(context,page);
  const market = page.getByRole("region", {name:"实时行情",exact:true});
  await expect(market).toContainText("行情源未连接");
  await expect(market.getByRole("table",{name:"实时报价"})).toContainText("行情时间");
  await expect(page.getByText("未选择合约",{exact:true})).toHaveCount(0);
  await expect(page.getByLabel("窗口联动组")).toHaveCount(0);
  await expect(page.getByRole("button",{name:"锁定合约",exact:true})).toHaveCount(0);
  await expect(page.getByRole("button",{name:"检查器",exact:true})).toHaveCount(0);
  await expect(page.getByLabel("预览数据版本")).toHaveCount(0);
  await expect(page.getByLabel("工作总览",{exact:true})).not.toContainText("AU2506");
  for (const width of [1440,1280]) {
    await page.setViewportSize({width,height:900});
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    await page.screenshot({path:`../../.state/dashboard-realtime-${width}.png`});
  }
  await page.getByRole("navigation",{name:"业务工作区"}).getByRole("button",{name:"市场",exact:true}).click();
  await expect(page.getByLabel("窗口联动组")).toBeVisible();
  await expect(page.getByRole("button",{name:"锁定合约",exact:true})).toBeVisible();
  expect(state.writes).toEqual([]);
});

test("empty dashboard retains market, combined account and risk without invented zero balances", async ({context,page}) => {
  await dashboard(context,page,true);
  for (const name of ["实时行情", "账户与持仓", "风险概览"]) await expect(page.getByRole("region",{name,exact:true})).toBeVisible();
  await expect(page.locator(".dashboard-tile")).toHaveCount(3);
  const account=page.getByRole("region",{name:"账户与持仓",exact:true});
  await expect(account.getByLabel("资金摘要").locator("dd")).toHaveText(["—","—","—","—"]);
  await expect(account.getByRole("table",{name:"持仓明细"})).toContainText("今仓");
  await expect(account).toContainText("持仓数据暂不可用");
  await expect(page.getByRole("button",{name:"查看接入状态 ↗",exact:true})).toHaveCount(1);
  const m=await page.getByRole("region",{name:"实时行情",exact:true}).boundingBox();
  const a=await account.boundingBox();
  expect(m!.x+m!.width).toBeLessThan(a!.x);
  expect(m!.height).toBeLessThan(230);
  await page.screenshot({path:"../../.state/dashboard-account-empty.png"});
  await expect(page.getByRole("button",{name:"同步或导入数据",exact:true})).toHaveCount(0);
});

test("edit mode supports optional widgets, cancellation, save and reload", async ({context,page}) => {
  await dashboard(context,page,true);
  await page.getByRole("button",{name:"编辑布局",exact:true}).click();
  await expect(page.getByRole("region",{name:"账户与持仓",exact:true})).toBeVisible();
  await page.getByRole("button",{name:"添加组件",exact:true}).click();
  const dialog=page.getByRole("dialog",{name:"添加组件"});
  await dialog.getByLabel("搜索组件").fill("新闻");
  await dialog.getByRole("button",{name:"添加市场新闻与公告",exact:true}).click();
  await dialog.getByRole("button",{name:"关闭",exact:true}).click();
  await page.getByRole("button",{name:"取消编辑",exact:true}).click();
  await expect(page.getByRole("region",{name:"市场新闻与公告",exact:true})).toHaveCount(0);
  await page.getByRole("button",{name:"编辑布局",exact:true}).click();
  await page.getByRole("button",{name:"移除实时行情",exact:true}).click();
  await page.getByRole("button",{name:/保存布局/}).click();
  await expect(page.getByRole("button",{name:"编辑布局",exact:true})).toBeVisible();
  await page.reload();
  await page.getByRole("button",{name:"编辑布局",exact:true}).click();
  await expect(page.getByRole("region",{name:"实时行情",exact:true})).toHaveCount(0);
  await page.getByRole("button",{name:"重置布局",exact:true}).click();
  await expect(page.getByRole("region",{name:"实时行情",exact:true})).toBeVisible();
});

test("read errors and task failures remain visible in a simplified dashboard", async ({context,page}) => {
  const state=await dashboard(context,page);
  await page.getByRole("button",{name:"提醒 1",exact:true}).click();
  const drawer=page.getByRole("dialog",{name:"异常与提醒"});
  await expect(drawer).toContainText("固定来源证据不可用");
  await page.keyboard.press("Escape");
  await expect(page.getByRole("button",{name:"提醒 1",exact:true})).toBeFocused();
  state.fail();
  await expect(page.getByText("状态待确认",{exact:true})).toBeVisible({timeout:10000});
  await expect(page.getByText("工作状态读取失败",{exact:false})).toContainText("上次读取的记录");
});

test("unsupported layouts are preserved without writeback", async ({context,page}) => {
  await context.addInitScript(() => localStorage.setItem("asterion.dashboard:dashboard@example.com",'{"version":"unsupported"}'));
  await dashboard(context,page,true);
  await expect(page.getByRole("alert").filter({hasText:"布局读取失败"})).toBeVisible();
  await page.getByRole("button",{name:"编辑布局",exact:true}).click();
  await expect(page.getByRole("button",{name:"保存布局",exact:true})).toBeDisabled();
  expect(await page.evaluate(() => localStorage.getItem("asterion.dashboard:dashboard@example.com"))).toBe('{"version":"unsupported"}');
});
