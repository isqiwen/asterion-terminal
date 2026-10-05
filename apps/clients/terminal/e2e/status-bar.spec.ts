import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";
test("compact footer exposes services, tasks and navigation", async ({ page }) => {
  await page.goto("/");
  const service = page
    .locator(".status-bar")
    .getByRole("button", { name: "查看服务连接", exact: true });
  await expect(service).toBeVisible();
  await expect(page.locator(".watchlist-workspace")).toBeVisible();
  await expect(page.getByRole("button", { name: "检查连接", exact: true })).toHaveCount(0);
  await service.click();
  const panel = page.getByRole("region", { name: "服务连接详情" });
  await expect(panel).toBeVisible();
  await expect(panel).not.toContainText("本机核心");
  await expect(panel).not.toContainText("本机服务");
  await expect(panel).toContainText("此电脑");
  await expect(panel).toContainText("行情服务");
  await expect(panel).toContainText("数据服务");
  await expect(panel.getByText("market-data", { exact: true })).not.toBeVisible();
  await panel.getByText("连接详情", { exact: true }).click();
  await expect(panel.getByText("market-data", { exact: true })).toBeVisible();
  await panel.getByText("连接详情", { exact: true }).click();
  await page.screenshot({ path: "apps/clients/terminal/test-results/status-bar-services.png" });
  await page.keyboard.press("Escape");
  await expect(panel).toHaveCount(0);
  await expect(service).toBeFocused();
  await service.click();
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await expect(panel).toHaveCount(0);
  await page
    .locator(".rail-actions")
    .getByRole("button", { name: "任务中心", exact: true })
    .click();
  await expect(page.getByRole("region", { name: "任务中心", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "关闭任务中心", exact: true }).click();
  await page
    .locator(".status-bar")
    .getByRole("button", { name: /项任务执行中/ })
    .click();
  await expect(page.getByRole("region", { name: "任务中心", exact: true })).toBeVisible();
  await service.click();
  page = await openSettingsWindow(
    page,
    panel.getByRole("button", { name: "连接设置", exact: true }),
  );
  await expect(page.getByRole("list", { name: "当前运行位置", exact: true })).toBeVisible();
});

test("revisioned polls accept unchanged replies from an idle core", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
    timeout: 60000,
  });
  // Exercise the real bridge module in the page, not a copy of its logic.
  const result = await page.evaluate(async () => {
    // Served by the Vite dev server; typed from the same source file.
    const path = "/src/bridge/client.ts";
    const client = (await import(/* @vite-ignore */ path)) as typeof import("../src/bridge/client");
    // A background refresh may publish once between the two calls while
    // services settle; an idle core then answers with the same revision.
    let first = await client.request("runtime.snapshot");
    let poll = await client.pollSnapshot(first.revision);
    for (let attempt = 0; attempt < 5 && !("unchanged" in poll); ++attempt) {
      await new Promise(resolve => setTimeout(resolve, 500));
      first = await client.request("runtime.snapshot");
      poll = await client.pollSnapshot(first.revision);
    }
    return { revision: first.revision, poll };
  });
  expect(result.poll).toMatchObject({ unchanged: true, revision: result.revision });
});

test("a service that has just started reads as starting, not as a fault", async ({ page }) => {
  // Startup itself waits for healthy services; this is a restart afterwards.
  let health = "ready";
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method !== "runtime.snapshot") return route.continue();
    // Ask for the whole snapshot: an "unchanged" reply has no services to alter.
    const response = await route.fetch({ postData: JSON.stringify({ ...request, params: {} }) });
    const body = await response.json();
    for (const node of body.result?.nodes ?? [])
      for (const service of node.health?.services ?? [])
        if (service.desired_running) {
          service.health = health;
          // As the Agent reports it: the missed first probe is the error text.
          service.error = health === "ready" ? "" : "IPC endpoint is not ready";
        }
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  const service = page
    .locator(".status-bar")
    .getByRole("button", { name: "查看服务连接", exact: true });
  await expect(service).toHaveText("● 服务");
  health = "starting";
  await expect(service).toContainText("服务启动中");
  await expect(service).not.toHaveClass(/bad|good/);
  health = "unresponsive";
  await expect(service).toContainText("服务异常");
  await expect(service).toHaveClass(/bad/);
  health = "ready";
  await expect(service).toHaveText("● 服务");
  await expect(service).toHaveClass(/good/);
});

test("log loss stays visible while a failed background refresh can recover", async ({ page }) => {
  let refreshFailed = false;
  let logFailures = 0;
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method !== "runtime.snapshot") return route.continue();
    const response = await route.fetch({ postData: JSON.stringify({ ...request, params: {} }) });
    const body = await response.json();
    if (body.result?.diagnostics) {
      body.result.diagnostics.log_failures = logFailures;
      body.result.diagnostics.refresh_failed = refreshFailed;
      body.result.diagnostics.refresh_failures = refreshFailed ? 1 : 0;
    }
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  const service = page.getByRole("button", { name: "查看服务连接", exact: true });
  await expect(service).toHaveClass(/good/);
  refreshFailed = true;
  logFailures = 3;
  await expect(service).toHaveText("● 服务待确认");
  await service.click();
  const panel = page.getByRole("region", { name: "服务连接详情" });
  await expect(panel).toContainText("后台状态刷新失败");
  await expect(panel).toContainText("本次运行有日志写入失败");
  refreshFailed = false;
  await expect(panel).not.toContainText("后台状态刷新失败");
  await expect(panel).toContainText("本次运行有日志写入失败");
  await expect(service).toHaveText("● 诊断不完整");
  await expect(service).toHaveClass(/bad/);
});

test("old account progress stays uncertain even while transport responds", async ({ page }) => {
  let stale = false;
  let serviceId = "";
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method !== "runtime.snapshot") return route.continue();
    const response = await route.fetch({ postData: JSON.stringify({ ...request, params: {} }) });
    const body = await response.json();
    const service = body.result?.nodes[0]?.health?.services[0];
    if (service) {
      serviceId = service.id;
      service.kind = "live";
      service.health = stale ? "degraded" : "ready";
      const idle = { observed: true, pending: false, age_ms: 0 };
      service.execution = {
        io: idle,
        state: { ...idle, pending: stale, age_ms: stale ? 60000 : 0 },
        persistence: { ...idle, age_ms: 60000 },
        initialization: idle,
        command: idle,
        business_ready: true,
      };
    }
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  await page.getByRole("button", { name: "查看服务连接", exact: true }).click();
  const panel = page.getByRole("region", { name: "服务连接详情" });
  await panel.getByText("连接详情", { exact: true }).click();
  const details = panel.locator(".service-connection-details > div").filter({
    has: page.getByText(serviceId, { exact: true }),
  });
  const readiness = details.getByText("业务就绪", { exact: true }).locator("..");
  const state = details.getByText("状态推进", { exact: true }).locator("..");
  await expect(readiness).toContainText("已就绪");
  await expect(details.getByText("持久化与准备", { exact: true }).locator("..")).toContainText(
    "空闲",
  );
  stale = true;
  await expect(state).toContainText("进展超时");
  await expect(readiness).toContainText("待确认");
  await expect(details.getByText("请求处理", { exact: true }).locator("..")).toContainText("正常");
});

test("a slow status read does not accumulate overlapping polls", async ({ page }) => {
  let enabled = false,
    polls = 0;
  let release!: () => void;
  const held = new Promise<void>(resolve => {
    release = resolve;
  });
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!enabled || request.method !== "runtime.snapshot" || !("since" in request.params))
      return route.continue();
    const index = polls++;
    const response = await route.fetch();
    if (index === 0) await held;
    await route.fulfill({ response });
  });
  try {
    await page.goto("/");
    const service = page.getByRole("button", { name: "查看服务连接", exact: true });
    await expect(service).toHaveText("● 服务", { timeout: 60000 });
    enabled = true;
    await expect.poll(() => polls).toBe(1);
    // Hold a read across several live-state polling intervals.
    await page.waitForTimeout(1600);
    expect(polls).toBe(1);
    release();
    await expect.poll(() => polls).toBeGreaterThan(1);
    await expect(service).toHaveText("● 服务");
  } finally {
    release();
  }
});
