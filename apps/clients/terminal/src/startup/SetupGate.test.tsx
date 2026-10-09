import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import {
  localHealth,
  localService,
  readySnapshot,
  serveCore,
  type CoreCall,
} from "../testing/core";
import { SetupGate } from "./SetupGate";

const start = () =>
  render(
    <SetupGate>
      <p>workbench</p>
    </SetupGate>,
  );
// The workbench opens by itself once every step has passed and there is
// nothing to point out.
const workbench = () => screen.queryByText("workbench");
const status = (step: string) =>
  screen.getByRole("progressbar", { name: step }).parentElement!.querySelector("span")!.textContent;
// The one step whose row reports a failure.
const failedStep = () => screen.getByText("失败").parentElement!.querySelector("b")!.textContent;
const retry = () => fireEvent.click(screen.getByRole("button", { name: "重试启动" }));
const serviceStarts = [
  "node.local",
  "market.local",
  "node.data_tasks.local.open",
  "node.data_tasks.local.create",
];
const sent = (calls: CoreCall[], ...methods: string[]) =>
  calls.map(call => call.method).filter(method => methods.includes(method));

test("Agent initialization failure stays on the Agent step and exposes its cause", async () => {
  let failure: null | { code: string; message: string } = null;
  const calls = serveCore(() => {
    const snapshot = readySnapshot();
    const health = localHealth(snapshot);
    health.phase = failure ? "recovery_required" : "initializing";
    health.failure = failure;
    return snapshot;
  });
  start();
  await waitFor(() => expect(status("服务管理器")).toBe("进行中"));
  expect(status("行情服务")).toBe("等待");
  failure = {
    code: "invalid_request",
    message:
      "Agent initialization failed; inspect node logs: unsupported managed service configuration version: test-node/services/market-data/service.json",
  };
  await waitFor(() => expect(failedStep()).toBe("服务管理器"));
  const alert = screen.getByRole("alert");
  expect(alert.textContent).toContain("Agent 初始化失败，请检查节点日志");
  fireEvent.click(within(alert).getByRole("button", { name: "详情" }));
  expect(alert.textContent).toContain("unsupported managed service configuration version");
  expect(sent(calls, "market.local")).toEqual([]);
});

test("startup lists unloadable plugins and interrupted tasks without blocking entry", async () => {
  serveCore(() => {
    const snapshot = readySnapshot();
    snapshot.native_plugins!.items.push({
      file: "broken.dylib",
      id: "",
      version: "",
      sha256: "f".repeat(64),
      capabilities: [],
      state: "invalid",
      error: "plugin entry point is missing",
    });
    snapshot.task_service!.interrupted_count = 1;
    return snapshot;
  });
  start();
  const notices = await screen.findByRole("list", { name: "进入前请留意" });
  expect(notices.textContent).toContain("插件 broken.dylib 无法加载");
  expect(notices.textContent).toContain("1 项任务上次被中断");
  // With something to point out, the screen waits for the user.
  expect(workbench()).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "进入工作台" }));
  expect(workbench()).not.toBeNull();
});

test("a Data/Task pair the Agent reports late is opened again, not created", async () => {
  // The Agent answers the first attachment before it has recovered its
  // service inventory.
  const calls = serveCore(({ method }) => {
    const snapshot = readySnapshot();
    if (method === "node.local") {
      const health = localHealth(snapshot);
      health.phase = "initializing";
      health.services = [];
    }
    return snapshot;
  });
  start();
  await waitFor(() => expect(workbench()).not.toBeNull());
  expect(sent(calls, "node.data_tasks.local.open", "node.data_tasks.local.create")).toEqual([
    "node.data_tasks.local.open",
  ]);
});

for (const [kind, id, label] of [
  ["data", "historical-data", "数据服务"],
  ["task", "task", "任务服务"],
] as const)
  test(`startup waits for the ${kind} heartbeat and reports its own failure`, async () => {
    let health: "starting" | "ready" | "failed" = "starting";
    serveCore(() => {
      const snapshot = readySnapshot();
      const service = localService(snapshot, id);
      if (health === "starting") {
        service.health = "starting";
        service.error = "IPC endpoint is not ready";
      }
      if (health === "failed") {
        service.health = "offline";
        service.state = "failed";
        service.error = "test: exited at start";
      }
      return snapshot;
    });
    const launch = start();
    // Each service needs its own heartbeat before its step completes.
    await waitFor(() => expect(status(label)).toBe("进行中"));
    expect(screen.getAllByRole("progressbar")).toHaveLength(5);
    expect(status("行情服务")).toBe("完成");
    if (kind === "task") expect(status("数据服务")).toBe("完成");
    expect(workbench()).toBeNull();
    health = "ready";
    await waitFor(() => expect(workbench()).not.toBeNull());
    // A service that failed to start fails its own step at once.
    health = "failed";
    launch.unmount();
    start();
    await waitFor(() => expect(failedStep()).toBe(label));
    expect(screen.getByRole("button", { name: "重试启动" })).toHaveProperty("disabled", false);
  });

test("startup reports the failing step, retries and opens the workbench once every step passes", async () => {
  const failing = new Map([
    ["node.local", "Agent start failed"],
    ["market.local", "Market start failed"],
    ["node.data_tasks.local.open", "Data/Task preparation failed"],
  ]);
  const calls = serveCore(({ method }) =>
    failing.has(method) ? { error: { message: failing.get(method)! } } : readySnapshot(),
  );
  let launch = start();
  // Startup runs by itself and stays on this screen while a step fails.
  const alert = await screen.findByRole("alert");
  expect(alert.textContent).toContain("操作失败");
  fireEvent.click(within(alert).getByRole("button", { name: "详情" }));
  expect(alert.textContent).toContain("Agent start failed");
  expect(failedStep()).toBe("服务管理器");
  fireEvent.change(screen.getByRole("combobox", { name: "语言" }), { target: { value: "en" } });
  expect(screen.getByRole("button", { name: "RETRY" })).toHaveProperty("disabled", false);
  fireEvent.change(screen.getByRole("combobox", { name: "Language" }), {
    target: { value: "zh" },
  });
  expect(workbench()).toBeNull();
  failing.delete("node.local");
  retry();
  await waitFor(() => expect(failedStep()).toBe("行情服务"));
  failing.delete("market.local");
  retry();
  await waitFor(() => expect(failedStep()).toBe("数据服务"));
  failing.delete("node.data_tasks.local.open");
  retry();
  await waitFor(() => expect(workbench()).not.toBeNull());
  // Every attempt starts again from the first step.
  expect(sent(calls, ...serviceStarts)).toEqual([
    "node.local",
    "node.local",
    "market.local",
    "node.local",
    "market.local",
    "node.data_tasks.local.open",
    "node.local",
    "market.local",
    "node.data_tasks.local.open",
  ]);
  // A later launch checks the services again before it opens the workbench.
  calls.length = 0;
  launch.unmount();
  launch = start();
  await waitFor(() => expect(workbench()).not.toBeNull());
  expect(sent(calls, ...serviceStarts)).toEqual([
    "node.local",
    "market.local",
    "node.data_tasks.local.open",
  ]);
  // Having opened before never skips the check of actual service health.
  failing.set("node.local", "Agent start failed");
  launch.unmount();
  start();
  await screen.findByRole("button", { name: "重试启动" });
  expect(workbench()).toBeNull();
});

for (const state of ["update_available", "recovery_required"] as const)
  test(`Agent ${state} is resolved automatically before service startup`, async () => {
    let upgraded = false;
    const calls = serveCore(({ method }) => {
      if (method === "node.agent.upgrade") upgraded = true;
      const snapshot = readySnapshot();
      snapshot.agent_program = {
        state: upgraded ? "current" : state,
        expected_digest: "a".repeat(64),
        installed_digest: "a".repeat(64),
        bundled_digest: "b".repeat(64),
      };
      return snapshot;
    });
    start();
    await waitFor(() => expect(workbench()).not.toBeNull());
    expect(sent(calls, "node.agent.upgrade", ...serviceStarts)).toEqual([
      "node.agent.upgrade",
      "node.local",
      "market.local",
      "node.data_tasks.local.open",
    ]);
    expect(calls.find(call => call.method === "node.agent.upgrade")!.params).toEqual({
      expected_digest: "a".repeat(64),
    });
  });

test("a service that went offline after every start fails its own step", async () => {
  let started = false;
  let online = false;
  serveCore(({ method }) => {
    if (method === "node.data_tasks.local.open") started = true;
    const snapshot = readySnapshot();
    if (started) snapshot.market!.transport_online = online;
    return snapshot;
  });
  start();
  await waitFor(() => expect(failedStep()).toBe("行情服务"));
  expect(workbench()).toBeNull();
  online = true;
  retry();
  await waitFor(() => expect(workbench()).not.toBeNull());
});

test("a service that never reports online fails its own step when the wait ends", async () => {
  serveCore(() => {
    const snapshot = readySnapshot();
    snapshot.task_service!.online = false;
    return snapshot;
  });
  vi.useFakeTimers();
  start();
  await act(() => vi.advanceTimersByTimeAsync(19000));
  expect(status("任务服务")).toBe("进行中");
  await act(() => vi.advanceTimersByTimeAsync(2000));
  expect(failedStep()).toBe("任务服务");
  expect(workbench()).toBeNull();
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
  test(`startup explains safe update deferral: ${diagnostic}`, async () => {
    const calls = serveCore(({ method }) => {
      if (method === "node.agent.upgrade")
        return { error: { code: "unavailable", message: diagnostic } };
      const snapshot = readySnapshot();
      snapshot.agent_program!.state = "update_available";
      return snapshot;
    });
    start();
    const alert = await screen.findByRole("alert");
    expect(alert.textContent).toContain(summary);
    expect(alert.textContent).not.toContain(diagnostic);
    fireEvent.click(within(alert).getByRole("button", { name: "详情" }));
    expect(alert.textContent).toContain(diagnostic);
    fireEvent.change(screen.getByRole("combobox", { name: "语言" }), { target: { value: "en" } });
    expect(alert.textContent).toContain(english);
    expect(sent(calls, "node.agent.upgrade", ...serviceStarts)).toEqual(["node.agent.upgrade"]);
    expect(screen.getByRole("button", { name: "RETRY" })).toHaveProperty("disabled", false);
  });
