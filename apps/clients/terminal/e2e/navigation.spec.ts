import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";

test("seven business modules and separate system administration preserve navigation without starting work", async ({
  page,
}) => {
  await page.goto("/");
  await expect(page.locator(".environment-label")).toHaveText("开发环境");
  await expect(page).toHaveTitle(/开发环境/);
  const methods: string[] = [];
  page.on("request", request => {
    if (request.url().endsWith("/__asterion/api")) methods.push(request.postDataJSON().method);
  });
  const nav = page.getByRole("navigation", { name: "业务工作区" });
  await expect(nav.getByRole("button")).toHaveText([
    "数据",
    "情报",
    "分析",
    "策略",
    "回测",
    "评估",
    "实盘",
  ]);
  await expect(nav.getByRole("button", { name: "数据", exact: true })).toHaveAttribute(
    "aria-current",
    "page",
  );
  await expect(page.getByRole("button", { name: "总览", exact: true })).toHaveCount(0);
  for (const [index, name] of ["数据", "情报", "分析", "策略", "回测", "评估", "实盘"].entries()) {
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
    page.locator(".workspace-tabs").getByRole("button", { name: "实盘", exact: true }),
  ).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("region", { name: "CTP 交易账户", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "系统管理", exact: true }).click();
  await expect(page.getByRole("navigation", { name: "业务子模块" }).getByRole("button")).toHaveText(
    ["安装与配置", "任务与节点", "资料与诊断"],
  );
  expect(
    methods.filter(method => method.startsWith("live.") || method.endsWith(".submit")),
  ).toEqual([]);
  await page.screenshot({ path: "apps/clients/terminal/test-results/navigation-system.png" });
});

test("unknown saved module starts at data", async ({ page }) => {
  await page.addInitScript(() =>
    localStorage.setItem("asterion.navigation", JSON.stringify({ module: "unknown.module" })),
  );
  await page.goto("/");
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }),
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
    data.result.task_service.failed_count = 1;
    data.result.task_service.tasks = [
      {
        id: "failed-download",
        kind: "minute_download",
        state: "failed",
        attempt: 1,
        data_source: "tushare.ft_mins",
        source_name: "Test download",
        instrument: "SHFE/rb/2026-10",
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
  await openView(page, "backtest");
  await expect(footer).toContainText("服务异常");
  await footer.getByRole("button", { name: /1 项任务异常/ }).click();
  const tasks = page.getByRole("region", { name: "任务中心", exact: true });
  await tasks.getByRole("button", { name: /Test download/ }).click();
  await expect(tasks).toHaveCount(0);
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }),
  ).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("navigation", { name: "业务子模块", exact: true })).toBeVisible();
  await page.screenshot({ path: "apps/clients/terminal/test-results/navigation-issues.png" });
  issues = false;
  await expect(footer).not.toContainText("1 项任务异常");
  await page.unrouteAll({ behavior: "wait" });
});
