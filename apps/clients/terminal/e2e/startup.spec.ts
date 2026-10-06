import { test, expect, type Page } from "./test";

test.use({ enterWorkbench: false });

// The workbench opens by itself once every step has passed and there is
// nothing to point out.
const workbench = (page: Page) =>
  page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true });
// The one step whose row reports a failure.
const failedStep = (page: Page) => page.locator(".setup-step").filter({ hasText: "失败" });

test("Agent initialization failure stays on the Agent step and exposes its cause", async ({
  page,
}) => {
  let fail = false;
  let serviceStarts = 0;
  await page.route("**/__asterion/api", async route => {
    const method = route.request().postDataJSON().method;
    if (method === "market.local") serviceStarts++;
    if (method !== "runtime.snapshot") return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    const node = body.result.nodes.find((node: { id: string }) => node.id === "local");
    node.health.phase = fail ? "recovery_required" : "initializing";
    node.health.failure = fail
      ? {
          code: "invalid_request",
          message:
            "Agent initialization failed; inspect node logs: unsupported managed service configuration version: test-node/services/market-data/service.json",
        }
      : null;
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  await expect(page.locator(".setup-step").filter({ hasText: "服务管理器" })).toContainText(
    "进行中",
  );
  await expect(page.locator(".setup-step").filter({ hasText: "行情服务" })).toContainText("等待");
  fail = true;
  await expect(failedStep(page)).toHaveText(/^服务管理器/);
  await expect(page.getByText("Agent 初始化失败，请检查节点日志", { exact: false })).toBeVisible();
  await page.getByRole("button", { name: "详情", exact: true }).click();
  await expect(page.locator(".error-diagnostic")).toContainText(
    "unsupported managed service configuration version",
  );
  expect(serviceStarts).toBe(0);
});

test("startup lists unloadable plugins and interrupted tasks without blocking entry", async ({
  page,
}) => {
  await page.route("**/__asterion/api", async route => {
    const body = route.request().postDataJSON();
    if (!["native.plugins.inspect", "runtime.snapshot"].includes(body.method))
      return route.continue();
    const response = await route.fetch();
    const value = await response.json();
    if (body.method === "native.plugins.inspect")
      value.result.native_plugins.items.push({
        file: "broken.dylib",
        id: "",
        version: "",
        sha256: "f".repeat(64),
        capabilities: [],
        state: "invalid",
        error: "plugin entry point is missing",
      });
    else if (value.result.task_service) value.result.task_service.interrupted_count = 1;
    return route.fulfill({ response, json: value });
  });
  await page.goto("/");
  const notices = page.getByRole("list", { name: "进入前请留意" });
  await expect(notices).toContainText("插件 broken.dylib 无法加载");
  await expect(notices).toContainText("1 项任务上次被中断");
  await expect(page.getByRole("button", { name: "进入工作台", exact: true })).toBeEnabled();
  // With something to point out, the screen waits for the user.
  await expect(workbench(page)).toHaveCount(0);
  // The workbench polls; let it reach the services without this route.
  await page.unrouteAll({ behavior: "wait" });
  await page.getByRole("button", { name: "进入工作台", exact: true }).click();
  await expect(workbench(page)).toBeVisible();
});

test("startup restores the Data/Task pair after the initial Agent inventory is still loading", async ({
  page,
  request,
}) => {
  const call = async (method: string, params: object = {}) => {
    const body = await (
      await request.post("/__asterion/api", { data: { version: 1, method, params } })
    ).json();
    if (body.error) throw new Error(`${method}: ${body.error.message}`);
    return body.result;
  };
  await call("node.action", { id: "local", service: "task", action: "stop" });
  await call("node.action", { id: "local", service: "historical-data", action: "stop" });
  await page.route("**/__asterion/api", async route => {
    if (route.request().postDataJSON().method !== "node.local") return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    const health = body.result.nodes.find((node: { id: string }) => node.id === "local").health;
    health.phase = "initializing";
    health.services = [];
    await route.fulfill({ response, json: body });
  });
  const taskService = async () =>
    (await call("runtime.snapshot")).nodes
      .find((node: { id: string }) => node.id === "local")
      .health.services.find((service: { id: string }) => service.id === "task");
  await expect.poll(async () => (await taskService()).desired_running).toBe(false);
  await page.goto("/");
  await expect(workbench(page)).toBeVisible({ timeout: 30000 });
  expect((await taskService()).health).toBe("ready");
  const restored = await call("runtime.snapshot");
  expect(restored.task_service.online).toBe(true);
  expect(restored.data.online).toBe(true);
});

for (const [kind, label] of [
  ["data", "数据服务"],
  ["task", "任务服务"],
] as const)
  test(`startup waits for the ${kind} heartbeat and reports its own failure`, async ({ page }) => {
    let health = "starting";
    await page.route("**/__asterion/api", async route => {
      if (route.request().postDataJSON().method !== "runtime.snapshot") return route.continue();
      const response = await route.fetch();
      const body = await response.json();
      for (const node of body.result?.nodes ?? [])
        for (const service of node.health?.services ?? [])
          if (service.kind === kind && service.desired_running) {
            service.health = health === "failed" ? "offline" : health;
            if (health === "starting") service.error = "IPC endpoint is not ready";
            if (health === "failed") {
              service.state = "failed";
              service.error = "test: exited at start";
            }
          }
      await route.fulfill({ response, json: body });
    });
    await page.goto("/");
    // Each service needs its own heartbeat before its step completes.
    const taskService = page.locator(".setup-step").filter({ hasText: label });
    await expect(taskService).toContainText("进行中");
    await expect(page.locator(".setup-step")).toHaveCount(5);
    if (kind === "task")
      await expect(page.locator(".setup-step").filter({ hasText: "数据服务" })).toContainText(
        "完成",
      );
    await expect(page.locator(".setup-step").filter({ hasText: "行情服务" })).toContainText("完成");
    await expect(workbench(page)).toHaveCount(0);
    health = "ready";
    await expect(workbench(page)).toBeVisible();
    // A service that failed to start fails its own step at once.
    health = "failed";
    await page.reload();
    await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeVisible();
    await expect(failedStep(page)).toHaveText(new RegExp(`^${label}`));
  });

test("startup reports the failing step, retries and opens the workbench once every step passes", async ({
  page,
}) => {
  let starts = 0;
  let fail = true;
  let failMarket = true;
  let marketStarts = 0;
  let failPair = true;
  let dataTaskStarts = 0;
  await page.route("**/__asterion/api", async route => {
    const body = route.request().postDataJSON();
    if (body.method === "node.local") {
      starts++;
      if (fail)
        return route.fulfill({
          status: 200,
          contentType: "application/json",
          body: JSON.stringify({ error: { message: "Agent start failed" } }),
        });
    }
    if (body.method === "market.local") {
      marketStarts++;
      if (failMarket)
        return route.fulfill({
          status: 200,
          contentType: "application/json",
          body: JSON.stringify({ error: { message: "Market start failed" } }),
        });
    }
    if (["node.data_tasks.local.open", "node.data_tasks.local.create"].includes(body.method)) {
      dataTaskStarts++;
      if (failPair)
        return route.fulfill({
          status: 200,
          contentType: "application/json",
          body: JSON.stringify({ error: { message: "Data/Task preparation failed" } }),
        });
    }
    return route.continue();
  });
  await page.goto("/");
  // Startup runs by itself and stays on this screen while a step fails.
  await expect(page.getByRole("alert")).toContainText("操作失败");
  await page.getByRole("alert").getByRole("button", { name: "详情" }).click();
  await expect(page.getByRole("alert")).toContainText("Agent start failed");
  await expect(failedStep(page)).toHaveText(/^服务管理器/);
  await page.getByRole("combobox", { name: "语言", exact: true }).selectOption("en");
  await expect(page.getByRole("button", { name: "RETRY", exact: true })).toBeVisible();
  await page.getByRole("combobox", { name: "Language", exact: true }).selectOption("zh");
  await page.screenshot({ path: "apps/clients/terminal/test-results/startup-failure.png" });
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }),
  ).toHaveCount(0);
  fail = false;
  await page.getByRole("button", { name: "重试启动", exact: true }).click();
  await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeEnabled();
  expect(marketStarts).toBe(1);
  await expect(failedStep(page)).toHaveText(/^行情服务/);
  failMarket = false;
  await page.getByRole("button", { name: "重试启动", exact: true }).click();
  await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeEnabled();
  expect(dataTaskStarts).toBe(1);
  await expect(failedStep(page)).toHaveText(/^数据服务/);
  failPair = false;
  await page.getByRole("button", { name: "重试启动", exact: true }).click();
  await expect(workbench(page)).toBeVisible();
  expect(starts).toBe(4);
  expect(marketStarts).toBe(3);
  expect(dataTaskStarts).toBe(2);
  // A later launch checks the services again before it opens the workbench.
  await page.reload();
  await expect(workbench(page)).toBeVisible();
  expect(starts).toBe(5);
  expect(marketStarts).toBe(4);
  expect(dataTaskStarts).toBe(3);
  // A saved marker never skips actual service health verification.
  fail = true;
  await page.reload();
  await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeVisible();
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }),
  ).toHaveCount(0);
});

for (const updateState of ["update_available", "recovery_required"])
  test(`Agent ${updateState} is resolved automatically before service startup`, async ({
    page,
  }) => {
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
        value.result.agent_program = {
          state: upgraded ? "current" : updateState,
          expected_digest: "a".repeat(64),
          installed_digest: "a".repeat(64),
          bundled_digest: "b".repeat(64),
        };
        return route.fulfill({ response, json: value });
      }
      if (body.method === "node.agent.upgrade") {
        upgradeCalls++;
        expect(body.params.expected_digest).toBe("a".repeat(64));
        upgraded = true;
        return route.fulfill({ json: inspectedResponse });
      }
      if (
        [
          "node.local",
          "market.local",
          "node.data_tasks.local.open",
          "node.data_tasks.local.create",
        ].includes(body.method)
      ) {
        expect(upgraded).toBe(true);
        serviceStarts++;
      }
      return route.continue();
    });
    await page.goto("/");
    await expect(workbench(page)).toBeVisible();
    expect(upgradeCalls).toBe(1);
    expect(serviceStarts).toBe(3);
    await expect(page.getByRole("button", { name: "升级服务管理器", exact: true })).toHaveCount(0);
  });

for (const failure of ["market", "task"] as const)
  test(`final startup probe rejects ${failure} state even after successful starts`, async ({
    page,
  }) => {
    let started = false;
    let fail = true;
    await page.route("**/__asterion/api", async route => {
      const body = route.request().postDataJSON();
      if (["node.data_tasks.local.open", "node.data_tasks.local.create"].includes(body.method))
        started = true;
      if (body.method === "runtime.snapshot" && started && fail) {
        const response = await route.fetch();
        const value = await response.json();
        if (failure === "market") value.result.market.transport_online = false;
        else value.result.task_service.online = false;
        return route.fulfill({ response, json: value });
      }
      return route.continue();
    });
    await page.goto("/");
    await expect(page.getByRole("button", { name: "重试启动", exact: true })).toBeVisible({
      timeout: 25000,
    });
    await expect(workbench(page)).toHaveCount(0);
    await expect(failedStep(page)).toHaveText({ market: /^行情服务/, task: /^任务服务/ }[failure]);
    fail = false;
    await page.getByRole("button", { name: "重试启动", exact: true }).click();
    await expect(workbench(page)).toBeVisible();
  });

for (const [diagnostic, summary, english] of [
  [
    "Agent upgrade is waiting for a recoverable service boundary: market: connected feed",
    "更新正在等待后台工作安全结束，请稍后重试",
    "The update is waiting for background work to finish safely. Retry later.",
  ],
  [
    "Agent upgrade validate: service lacks an automatic upgrade recovery boundary: paper.local",
    "正在运行的服务尚不支持自动更新，请保留当前工作并稍后重试",
    "A running service does not yet support automatic updates. Keep your current work running and retry later.",
  ],
])
  test(`startup explains safe update deferral: ${diagnostic}`, async ({ page }) => {
    let upgradeCalls = 0;
    let serviceStarts = 0;
    await page.route("**/__asterion/api", async route => {
      const body = route.request().postDataJSON();
      if (body.method === "node.agent.inspect") {
        const response = await route.fetch();
        const value = await response.json();
        value.result.agent_program = {
          state: "update_available",
          expected_digest: "a".repeat(64),
        };
        return route.fulfill({ response, json: value });
      }
      if (body.method === "node.agent.upgrade") {
        upgradeCalls++;
        return route.fulfill({
          json: { error: { code: "unavailable", message: diagnostic } },
        });
      }
      if (
        [
          "node.local",
          "market.local",
          "node.data_tasks.local.open",
          "node.data_tasks.local.create",
        ].includes(body.method)
      )
        serviceStarts++;
      return route.continue();
    });
    await page.goto("/");
    const alert = page.getByRole("alert");
    await expect(alert).toContainText(summary);
    await expect(alert).not.toContainText(diagnostic);
    await alert.getByRole("button", { name: "详情", exact: true }).click();
    await expect(alert).toContainText(diagnostic);
    await page.getByRole("combobox", { name: "语言", exact: true }).selectOption("en");
    await expect(alert).toContainText(english);
    expect(upgradeCalls).toBe(1);
    expect(serviceStarts).toBe(0);
    await expect(page.getByRole("button", { name: "RETRY", exact: true })).toBeEnabled();
  });
