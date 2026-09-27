import { test, expect } from "@playwright/test";

test.use({ storageState: { cookies: [], origins: [] } });
test("fresh setup requires consent, reports real failure, retries and persists completion", async ({ page }) => {
  let starts = 0;
  let fail = true;
  let failMarket = true;
  let marketStarts = 0;
  let failResearch = true;
  let researchStarts = 0;
  await page.route("**/__asterion/api", async route => {
    const body = route.request().postDataJSON();
    if (body.method === "node.local") {
      starts++;
      if (fail) return route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({error:{message:"Agent start failed"}}) });
    }
    if (body.method === "market.local") {
      marketStarts++;
      if (failMarket) return route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({error:{message:"Market start failed"}}) });
    }
    if (body.method === "research.local") {
      researchStarts++;
      if (failResearch) return route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({error:{message:"Research start failed"}}) });
    }
    return route.continue();
  });
  await page.goto("/");
  await expect(page.getByRole("button", { name: "开始设置", exact: true })).toBeEnabled();
  expect(starts).toBe(0);
  await expect(page.getByRole("button", { name: "总览", exact: true })).toHaveCount(0);
  await page.getByRole("combobox", { name: "语言", exact: true }).selectOption("en");
  await expect(page.getByRole("button", { name: "BEGIN SETUP", exact: true })).toBeVisible();
  await page.getByRole("combobox", { name: "Language", exact: true }).selectOption("zh");
  await page.screenshot({ path: "apps/terminal/test-results/first-setup.png" });
  await page.getByRole("button", { name: "开始设置", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("操作失败");
  await page.getByRole("alert").getByRole("button", { name: "详情" }).click();
  await expect(page.getByRole("alert")).toContainText("Agent start failed");
  expect(await page.evaluate(() => localStorage.getItem("asterion.setup.completed.v1"))).toBeNull();
  await expect(page.getByRole("button", { name: "总览", exact: true })).toHaveCount(0);
  fail = false;
  await page.getByRole("button", { name: "重试启动", exact: true }).click();
  await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeEnabled();
  expect(marketStarts).toBe(1);
  expect(await page.evaluate(() => localStorage.getItem("asterion.setup.completed.v1"))).toBeNull();
  failMarket = false;
  await page.getByRole("button", { name: "重试启动", exact: true }).click();
  await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeEnabled();
  expect(researchStarts).toBe(1);
  expect(await page.evaluate(() => localStorage.getItem("asterion.setup.completed.v1"))).toBeNull();
  failResearch = false;
  await page.getByRole("button", { name: "重试启动", exact: true }).click();
  await expect(page.getByRole("button", { name: "进入工作台", exact: true })).toBeEnabled();
  expect(starts).toBe(4);
  expect(marketStarts).toBe(3);
  expect(researchStarts).toBe(2);
  await expect(page.getByRole("progressbar", { name: "验证服务连接" })).toHaveAttribute("aria-valuenow", "100");
  await page.getByRole("button", { name: "进入工作台", exact: true }).click();
  await expect(page.getByRole("button", { name: "总览", exact: true })).toBeVisible();
  await page.reload();
  await expect(page.getByRole("button", { name: "总览", exact: true })).toBeVisible();
  expect(starts).toBe(5);
  expect(marketStarts).toBe(4);
  expect(researchStarts).toBe(3);
  // A saved marker never skips actual service health verification.
  fail = true;
  await page.reload();
  await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeVisible();
  await expect(page.getByRole("button", { name: "总览", exact: true })).toHaveCount(0);
});

for (const updateState of ["update_available", "recovery_required"]) test(`Agent ${updateState} is resolved automatically before service startup`, async ({ page }) => {
  let upgraded = false;
  let serviceStarts = 0;
  let upgradeCalls = 0;
  let inspectedResponse: unknown;
  await page.route("**/__asterion/api", async route => {
    const body = route.request().postDataJSON();
    if (body.method === "node.agent.inspect") {
      const response = await route.fetch();
      const value = await response.json();
      inspectedResponse = value;
      value.result.agent_program = { state: upgraded ? "current" : updateState, expected_digest: "a".repeat(64), installed_digest: "a".repeat(64), bundled_digest: "b".repeat(64) };
      return route.fulfill({ response, json: value });
    }
    if (body.method === "node.agent.upgrade") {
      upgradeCalls++;
      expect(body.params.expected_digest).toBe("a".repeat(64));
      upgraded = true;
      return route.fulfill({ json: inspectedResponse });
    }
    if (["node.local", "market.local", "research.local"].includes(body.method)) {
      expect(upgraded).toBe(true);
      serviceStarts++;
    }
    return route.continue();
  });
  await page.goto("/");
  await page.getByRole("button", { name: "开始设置", exact: true }).click();
  await expect(page.getByRole("button", { name: "进入工作台", exact: true })).toBeEnabled();
  expect(upgradeCalls).toBe(1);
  expect(serviceStarts).toBe(3);
  await expect(page.getByRole("button", { name: "升级 Agent", exact: true })).toHaveCount(0);
});
