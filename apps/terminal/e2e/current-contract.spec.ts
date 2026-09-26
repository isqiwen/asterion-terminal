import { expect, test } from "@playwright/test";
import { parseLayout } from "@asterion/ui-terminal-workspace/layout";
import { nativeContext } from "./native";

test("unknown browser layout schema reports an error without overwriting input", async ({ page }) => {
  const raw = JSON.stringify({ ...parseLayout(null), version: "unknown-schema" });
  await page.addInitScript((value) => localStorage.setItem("asterion.layout", value), raw);
  await page.goto("/");
  await expect(page.getByText("工作区布局版本不受支持", { exact: false })).toBeVisible();
  expect(await page.evaluate(() => localStorage.getItem("asterion.layout"))).toBe(raw);
});

test("missing native layout fields block writes", async ({ page, context }) => {
  let writes = 0;
  const layout = JSON.parse(JSON.stringify({ ...parseLayout(null), dock: undefined }));
  await nativeContext(context, {
    desktop_workspace_read: () => ({ id: "main", revision: 0, layout }),
    desktop_workspace_patch: () => { writes++; throw new Error("Unexpected state rewrite"); },
  });
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });
 return route.fulfill({json:{status:"ready"}}); });
  await page.goto("/");
  await expect(page.getByText("工作区布局不符合当前规范", { exact: false })).toBeVisible();
  expect(writes).toBe(0);
});

test("provider responses without current lifecycle state cannot enable sync", async ({ page }) => {
  await page.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    return route.fulfill({ json: path.endsWith("/data/providers")
      ? [{id:"tushare", name:"Unsupported provider", api_version:2, configured:true, capabilities:[]}]
      : path.endsWith("/health") ? {status:"ready"} : [] });
  });
  await page.goto("/");
  await page.getByLabel("会话令牌").fill("test-current-contract-token");
  await page.locator("form").filter({ has: page.getByLabel("会话令牌") }).getByRole("button", {name:"连接",exact:true}).click();
  await page.getByRole("navigation", {name:"业务工作区"}).getByRole("button", {name:"数据",exact:true}).click();
  await expect(page.getByText("数据源状态不符合当前接口规范", {exact:false})).toBeVisible();
  await expect(page.getByRole("option", {name:"Unsupported provider"})).toHaveCount(0);
});
