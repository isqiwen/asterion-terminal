import { test, expect } from "./test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";

test("six workspaces restore the last destination without a dashboard", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".environment-label")).toHaveText("开发环境");
  await expect(page).toHaveTitle(/开发环境/);
  const nav = page.getByRole("navigation", { name: "业务工作区" });
  await expect(nav.getByRole("button")).toHaveText([
    "自选",
    "合约",
    "市场",
    "数据",
    "研究",
    "交易",
  ]);
  await expect(nav.getByRole("button", { name: "自选", exact: true })).toHaveAttribute(
    "aria-current",
    "page",
  );
  await expect(page.getByRole("button", { name: "总览", exact: true })).toHaveCount(0);
  for (const [index, name] of ["自选", "合约", "市场", "数据", "研究", "交易"].entries()) {
    await page.keyboard.press(`Control+${index + 1}`);
    await expect(nav.getByRole("button", { name, exact: true })).toHaveAttribute(
      "aria-current",
      "page",
    );
  }
  const settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "偏好设置", exact: true }).click();
  page = await closeSettingsWindow(settings);
  await page.reload();
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }),
  ).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("region", { name: "CTP 交易账户", exact: true })).toBeVisible();
  await page.screenshot({ path: "apps/clients/terminal/test-results/navigation-trading.png" });
});

test("unknown saved workspace starts at watchlist", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("asterion.workspace", "unknown.workspace"));
  await page.goto("/");
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }),
  ).toHaveAttribute("aria-current", "page");
});

test("service and task failures remain visible from every workspace", async ({ page }) => {
  let issues = false;
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method !== "runtime.snapshot" || !issues) return route.continue();
    const response = await route.fetch({ postData: { ...request, params: {} } });
    const data = await response.json();
    const local = data.result.nodes.find((node: { id: string }) => node.id === "local");
    local.health.services[0] = {
      ...local.health.services[0],
      desired_running: true,
      state: "stopped",
      health: "unavailable",
      error: "Test-only service failure",
    };
    data.result.research.tasks = [
      {
        id: "failed-download",
        kind: "minute_download",
        state: "failed",
        attempt: 1,
        data_source: "tushare.fut_min",
        source_name: "Test download",
        instrument: "SHFE/rb2610",
        completed: 0,
        total: 1,
        error: "Test-only download failure",
        result_digest: "",
        trading_day: "",
        submission_sequence: 1,
        submitted_at_ms: 1,
        updated_at_ms: 1,
      },
    ];
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  await expect(page.getByRole("navigation", { name: "业务工作区" })).toBeVisible();
  issues = true;
  const footer = page.locator(".status-bar");
  await expect(footer).toContainText("服务异常");
  await expect(footer).toContainText("1 项任务异常");
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(footer).toContainText("服务异常");
  await footer.getByRole("button", { name: /1 项任务异常/ }).click();
  const tasks = page.getByRole("region", { name: "任务中心", exact: true });
  await tasks.getByRole("button", { name: /Test download/ }).click();
  await expect(tasks).toHaveCount(0);
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }),
  ).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("navigation", { name: "数据页面", exact: true })).toBeVisible();
  await page.screenshot({ path: "apps/clients/terminal/test-results/navigation-issues.png" });
  issues = false;
  await expect(footer).not.toContainText("1 项任务异常");
  await page.unrouteAll({ behavior: "ignoreErrors" });
});
