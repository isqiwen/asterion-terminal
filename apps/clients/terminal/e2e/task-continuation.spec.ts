import type { TaskRecord } from "../src/bridge/client";
import { rpc, seedDataset } from "./dataset-fixture";
import { closeSettingsWindow, openSettingsWindow } from "./settings-helper";
import { checkSnapshot } from "./snapshot-schema";
import { test, expect } from "./test";

test("a fresh local task store preserves the original tasks and requires an explicit switch", async ({
  page,
}) => {
  test.setTimeout(90000);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  await seedDataset(
    page.request,
    Array.from({ length: 40 }, (_, i) => 100 + i + (i % 3)),
    "continuation-original",
  );
  await rpc(page.request, "factor.submit", {
    id: "retained-factor",
    lookbacks: [2],
    horizon: 1,
    evaluation: { mode: "full_sample" },
  });
  await expect
    .poll(async () => {
      const snapshot = await rpc(page.request, "runtime.snapshot");
      return snapshot.task_service?.tasks.find((task: TaskRecord) => task.id === "retained-factor")
        ?.state;
    })
    .toBe("succeeded");
  const original = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("original task store", original);
  const originalService = original.task_service!.service;
  const originalTasks = original.task_service!.tasks;
  expect(originalTasks.length).toBeGreaterThan(0);
  expect(original.datasets).toHaveLength(1);
  const reattached = await rpc(page.request, "node.data_tasks.attach", {
    id: "local",
    service: originalService,
  });
  expect(reattached.datasets).toEqual(original.datasets);
  expect((await rpc(page.request, "node.data_tasks.local.open")).datasets).toEqual(
    original.datasets,
  );
  const name = "backtest-factor-continued";
  for (const params of [
    { id: "local", service: name, kind: "market", port: "0" },
    { id: "local", service: name, kind: "task", port: "7443" },
    { id: "local", service: originalService, kind: "task", port: "0" },
  ]) {
    const response = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "node.deploy", params },
    });
    expect((await response.json()).error).toBeDefined();
  }
  let settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "连接与部署", exact: true }).click();
  await settings
    .getByRole("list", { name: "当前运行位置" })
    .getByRole("listitem")
    .filter({ hasText: "任务服务" })
    .getByRole("button", { name: "管理", exact: true })
    .click();
  await settings.getByRole("button", { name: "部署服务", exact: true }).click();
  const form = settings.getByRole("form", { name: "部署服务" });
  await expect(form).toContainText("已有实例完整保留");
  await expect(form.getByLabel("服务端口")).toHaveCount(0);
  await form.getByLabel("新服务名称").fill(name);
  const deployed = settings.waitForResponse(
    response =>
      response.url().endsWith("/__asterion/api") &&
      response.request().postDataJSON()?.method === "node.deploy",
  );
  await form.getByRole("button", { name: "部署服务", exact: true }).click();
  // Publishing the service programs includes durable I/O; assert the outcome
  // after its response, within this workflow's overall deadline.
  expect((await (await deployed).json()).error).toBeUndefined();
  await expect(settings.getByRole("status")).toContainText("当前运行位置未改变");
  expect((await rpc(page.request, "runtime.snapshot")).task_service.service).toBe(originalService);
  expect((await rpc(page.request, "runtime.snapshot")).datasets).toEqual(original.datasets);
  // Keeping the original store does not require both pairs to run concurrently.
  // Release its node reservation before using the newly deployed pair.
  for (const service of [originalService, original.data!.service])
    await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
  const use = async (service: string) => {
    const row = settings.getByRole("listitem", { name: service, exact: true });
    await row.getByRole("button", { name: "使用此服务", exact: true }).click();
    await settings
      .getByRole("dialog", { name: "切换运行位置" })
      .getByRole("button", { name: "确认操作", exact: true })
      .click();
    await expect(row).toContainText("当前使用");
    // Selection can precede service recovery; assert data only after this exact
    // instance reports ready, without resubmitting the switch or deployment.
    await expect
      .poll(async () => {
        const selected = (await rpc(page.request, "runtime.snapshot")).task_service;
        return selected?.online ? selected.service : null;
      })
      .toBe(service);
  };
  await use(name);
  const fresh = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("fresh continuation store", fresh);
  expect(fresh.task_service!.service).toBe(name);
  expect(fresh.task_service!.tasks).toEqual([]);
  expect(fresh.datasets).toEqual([]);
  expect(fresh.task_service!.capacity).toEqual({
    retained_tasks: 0,
    active_used: 0,
    active_reserved: 0,
    uncommitted: 0,
    active_limit: 1000,
  });
  await closeSettingsWindow(settings);
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByLabel("活动任务容量")).toBeVisible();
  await rpc(page.request, "node.action", { id: "local", service: name, action: "stop" });
  await page.getByRole("button", { name: "重试准备", exact: true }).click();
  await expect
    .poll(async () => {
      const taskService = (await rpc(page.request, "runtime.snapshot")).task_service;
      return taskService?.online ? taskService.service : "offline";
    })
    .toBe(name);
  settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "连接与部署", exact: true }).click();
  await settings
    .getByRole("list", { name: "当前运行位置" })
    .getByRole("listitem")
    .filter({ hasText: "任务服务" })
    .getByRole("button", { name: "管理", exact: true })
    .click();
  for (const service of [name, fresh.data!.service])
    await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
  for (const service of [original.data!.service, originalService])
    await rpc(page.request, "node.action", { id: "local", service, action: "start" });
  await use(originalService);
  const restored = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("original task store remains accessible", restored);
  expect(restored.task_service!.tasks).toEqual(originalTasks);
  expect(restored.datasets).toEqual([]);
  expect(restored.task_service!.capacity!.retained_tasks).toBe(originalTasks.length);
});
