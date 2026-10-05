import { test, expect } from "./test";
import { openSettingsWindow } from "./settings-helper";
import type { NodeStatus, Snapshot } from "../src/bridge/client";

test("remote deployment stays separate from use; switching and stopping name their exact target", async ({
  page,
}) => {
  await page.goto("/");
  await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
    timeout: 60000,
  });
  const response = await page.request.post("/__asterion/api", {
    data: { version: 1, method: "runtime.snapshot", params: {} },
  });
  const base = (await response.json()).result as Snapshot;
  const remote: NodeStatus = {
    ...structuredClone(base.nodes.find(n => n.id === "local")!),
    id: "compute-1",
    host: "192.0.2.10",
    port: 7442,
  };
  remote.health!.services = [];
  let current = structuredClone(base);
  let revision = base.revision;
  const mutations: { method: string; params: Record<string, string> }[] = [];
  page = await openSettingsWindow(page);
  await page.route("**/__asterion/api", async route => {
    const { method, params } = route.request().postDataJSON();
    if (
      ![
        "runtime.snapshot",
        "node.local",
        "node.deploy",
        "node.data_tasks.attach",
        "node.action",
      ].includes(method)
    )
      return route.continue();
    if (["node.deploy", "node.data_tasks.attach", "node.action"].includes(method))
      mutations.push({ method, params });
    if (method === "node.deploy") {
      expect(params.id).toBe(remote.id);
      const service = structuredClone(
        base.nodes.find(n => n.id === "local")!.health!.services.find(s => s.kind === "task")!,
      );
      Object.assign(service, {
        id: params.service,
        port: Number(params.port),
        active_workers: 2,
        endpoint: "",
        directory: "/test/remote/taskService",
        revision: "remote-revision",
      });
      remote.health!.services.push(service);
    }
    if (method === "node.data_tasks.attach")
      current.task_service = {
        ...base.task_service!,
        remote: true,
        host: remote.host,
        port: 7550,
        service: "task",
        connection_id: "test-remote-taskService",
        online: true,
      };
    if (method === "node.action") {
      expect(params).toEqual({ id: "compute-1", service: "task", action: "stop" });
      Object.assign(remote.health!.services[0], {
        state: "stopped",
        desired_running: false,
        health: "offline",
        active_workers: 0,
      });
    }
    current = {
      ...current,
      nodes: [...base.nodes, remote],
      revision: ++revision,
      refreshed_at_ms: Date.now(),
    };
    return route.fulfill({ json: { result: current } });
  });
  await page.getByRole("button", { name: "连接与部署", exact: true }).click();
  const location = page
    .getByRole("list", { name: "当前运行位置" })
    .getByRole("listitem")
    .filter({ hasText: "任务服务" });
  await expect(location).toContainText("本机");
  await page.getByRole("button", { name: "机器管理", exact: true }).click();
  await page.getByRole("button", { name: "compute-1 在线", exact: true }).click();
  await page.getByRole("button", { name: "部署服务", exact: true }).click();
  await page.getByLabel("新服务名称", { exact: true }).fill("task");
  await page.getByLabel("服务端口", { exact: true }).fill("7550");
  await page.getByRole("button", { name: "上传并部署", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("当前运行位置未改变");
  const service = page.getByRole("listitem", { name: "task", exact: true });
  await expect(service.getByRole("button", { name: "使用此服务" })).toBeVisible();
  expect(mutations.map(m => m.method)).toEqual(["node.deploy"]);
  await page.getByRole("button", { name: "当前运行位置", exact: true }).click();
  await expect(location).toContainText("本机");
  await page.getByRole("button", { name: "机器管理", exact: true }).click();
  await service.getByRole("button", { name: "使用此服务" }).click();
  const dialog = page.getByRole("dialog", { name: "切换运行位置" });
  await expect(dialog).toContainText("compute-1 / task");
  await expect(dialog).toContainText("已有数据和任务不会搬到目标机器");
  await dialog.getByRole("button", { name: "取消", exact: true }).click();
  expect(mutations).toHaveLength(1);
  await service.getByRole("button", { name: "使用此服务" }).click();
  await dialog.getByRole("button", { name: "确认操作" }).click();
  await expect(service).toContainText("当前使用");
  await page.getByRole("button", { name: "当前运行位置", exact: true }).click();
  await expect(location).toContainText("compute-1");
  await page.getByRole("button", { name: "机器管理", exact: true }).click();
  await service.getByText("管理服务", { exact: true }).click();
  await service.getByRole("button", { name: "停止", exact: true }).click();
  const stop = page.getByRole("dialog", { name: "停止服务" });
  await expect(stop).toContainText("当前工作进程：2");
  await page.screenshot({ path: "apps/clients/terminal/test-results/deployment-stop.png" });
  await stop.getByRole("button", { name: "取消", exact: true }).click();
  expect(mutations.filter(m => m.method === "node.action")).toHaveLength(0);
  await service.getByRole("button", { name: "停止", exact: true }).click();
  await stop.getByRole("button", { name: "确认操作" }).click();
  await expect(service).toContainText("已停止");
  expect(mutations.filter(m => m.method === "node.action")).toHaveLength(1);
  remote.state = "unreachable";
  await expect(page.getByRole("alert")).toContainText("节点失联");
  await expect(service.getByRole("button", { name: "启动", exact: true })).toBeDisabled();
  await page.setViewportSize({ width: 640, height: 600 });
  await page.screenshot({ path: "apps/clients/terminal/test-results/deployment-machines.png" });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
