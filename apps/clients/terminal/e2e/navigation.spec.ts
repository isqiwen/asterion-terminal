import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";

test("four business modules preserve navigation without starting work", async ({ page }) => {
  await page.goto("/");
  const methods: { method: string; params: { request?: Record<string, object> } }[] = [];
  page.on("request", request => {
    if (request.url().endsWith("/__asterion/api")) methods.push(request.postDataJSON());
  });
  // Opening a page only reads accounts: their state, health and strategy packages.
  const reads = ({ request }: (typeof methods)[number]["params"]) =>
    !!request && ("getAccount" in request || "getHealth" in request || "getStrategies" in request);
  const nav = page.getByRole("navigation", { name: "业务工作区" });
  const names = ["行情", "交易", "研究", "数据"];
  await expect(nav.getByRole("button")).toHaveText(names);
  await expect(nav.getByRole("button", { name: "行情", exact: true })).toHaveAttribute(
    "aria-current",
    "page",
  );
  for (const [index, name] of names.entries()) {
    await page.keyboard.press(`Control+${index + 1}`);
    await expect(nav.getByRole("button", { name, exact: true })).toHaveAttribute(
      "aria-current",
      "page",
    );
  }
  await openView(page, "trading");
  const settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "偏好设置", exact: true }).click();
  page = await closeSettingsWindow(settings);
  await page.reload();
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }),
  ).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("region", { name: "CTP 交易账户", exact: true })).toBeVisible();
  await expect(page.getByRole("navigation", { name: "子模块视图" }).getByRole("button")).toHaveText(
    ["账户", "策略"],
  );
  await openView(page, "coverage");
  await expect(page.getByRole("navigation", { name: "子模块视图" }).getByRole("button")).toHaveText(
    ["概览", "覆盖", "浏览"],
  );
  expect(
    methods.filter(
      ({ method, params }) =>
        method.endsWith(".submit") || (method === "trading.call" && !reads(params)),
    ),
  ).toEqual([]);
  await page.screenshot({ path: "apps/clients/terminal/test-results/navigation-system.png" });
});

test("unknown saved module starts at the market", async ({ page }) => {
  await page.addInitScript(() =>
    localStorage.setItem("asterion.navigation", JSON.stringify({ module: "unknown.module" })),
  );
  await page.goto("/");
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "行情", exact: true }),
  ).toHaveAttribute("aria-current", "page");
});

test("a service failure remains visible from every workspace", async ({ page }) => {
  let issues = false;
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method !== "runtime.snapshot" || !issues) return route.continue();
    const response = await route.fetch({ postData: { ...request, params: {} } });
    const data = await response.json();
    data.result.lab = { state: "restarting", pid: 0, restarts: 1, error: "Test-only failure" };
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  await expect(page.getByRole("navigation", { name: "业务工作区" })).toBeVisible();
  issues = true;
  const footer = page.locator(".status-bar");
  await expect(footer).toContainText("研究 重启中");
  await openView(page, "research");
  await expect(footer).toContainText("研究 重启中");
  await openView(page, "market");
  await expect(footer).toContainText("研究 重启中");
  await page.screenshot({ path: "apps/clients/terminal/test-results/navigation-issues.png" });
  issues = false;
  await expect(footer).not.toContainText("研究 重启中");
  await page.unrouteAll({ behavior: "wait" });
});
