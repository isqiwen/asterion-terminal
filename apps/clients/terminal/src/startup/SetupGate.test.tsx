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
const serviceStarts = ["market.local", "node.research.local.open", "node.research.local.create"];
const sent = (calls: CoreCall[], ...methods: string[]) =>
  calls.map(call => call.method).filter(method => methods.includes(method));

test("startup lists unloadable plugins and interrupted tasks without blocking entry", async () => {
  serveCore(() => {
    const snapshot = readySnapshot();
    snapshot.native_plugins!.items.push({
      file: "broken.dylib",
      id: "",
      version: "",
      sha256: "f".repeat(64),
      capabilities: [],
      selectable: false,
      state: "invalid",
      error: "plugin entry point is missing",
    });
    snapshot.research_service!.interrupted_count = 1;
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

for (const [kind, id, label] of [
  ["data", "historical-data", "恢复已有工作区"],
  ["research", "research", "恢复已有工作区"],
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
    expect(screen.getAllByRole("progressbar")).toHaveLength(3);
    expect(status("行情服务")).toBe("完成");

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
    ["market.local", "Market start failed"],
    ["node.research.local.open", "Data/Task preparation failed"],
  ]);
  const calls = serveCore(({ method }) =>
    failing.has(method) ? { error: { message: failing.get(method)! } } : readySnapshot(),
  );
  let launch = start();
  // Startup runs by itself and stays on this screen while a step fails.
  const alert = await screen.findByRole("alert");
  expect(alert.textContent).toContain("操作失败");
  fireEvent.click(within(alert).getByRole("button", { name: "详情" }));
  expect(alert.textContent).toContain("Market start failed");
  expect(failedStep()).toBe("行情服务");
  fireEvent.change(screen.getByRole("combobox", { name: "语言" }), { target: { value: "en" } });
  expect(screen.getByRole("button", { name: "RETRY" })).toHaveProperty("disabled", false);
  fireEvent.change(screen.getByRole("combobox", { name: "Language" }), {
    target: { value: "zh" },
  });
  expect(workbench()).toBeNull();
  failing.delete("market.local");
  retry();
  await waitFor(() => expect(failedStep()).toBe("恢复已有工作区"));
  failing.delete("node.research.local.open");
  retry();
  await waitFor(() => expect(workbench()).not.toBeNull());
  // Every attempt starts again from the first step.
  expect(sent(calls, ...serviceStarts)).toEqual([
    "market.local",
    "market.local",
    "node.research.local.open",
    "market.local",
    "node.research.local.open",
  ]);
  // A later launch checks the services again before it opens the workbench.
  calls.length = 0;
  launch.unmount();
  launch = start();
  await waitFor(() => expect(workbench()).not.toBeNull());
  expect(sent(calls, ...serviceStarts)).toEqual(["market.local", "node.research.local.open"]);
  // Having opened before never skips the check of actual service health.
  failing.set("market.local", "Market start failed");
  launch.unmount();
  start();
  await screen.findByRole("button", { name: "重试启动" });
  expect(workbench()).toBeNull();
});

test("a service that went offline after every start fails its own step", async () => {
  let started = false;
  let online = false;
  serveCore(({ method }) => {
    if (method === "node.research.local.open") started = true;
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
    snapshot.research_service!.online = false;
    return snapshot;
  });
  vi.useFakeTimers();
  start();
  await act(() => vi.advanceTimersByTimeAsync(19000));
  expect(status("恢复已有工作区")).toBe("进行中");
  await act(() => vi.advanceTimersByTimeAsync(2000));
  expect(failedStep()).toBe("恢复已有工作区");
  expect(workbench()).toBeNull();
});

test("Research restores without a Data deployment or connection", async () => {
  const calls = serveCore(() => {
    const snapshot = readySnapshot();
    snapshot.data = null;
    const health = localHealth(snapshot);
    health.services = health.services.filter(service => service.kind !== "data");
    return snapshot;
  });
  start();
  await waitFor(() => expect(workbench()).not.toBeNull());
  expect(sent(calls, "node.research.local.open")).toEqual(["node.research.local.open"]);
  expect(sent(calls, "node.data.local.open")).toEqual([]);
});

test("an explicitly stopped local Market preserves intent and permits independent Data and Research work", async () => {
  const calls = serveCore(() => {
    const snapshot = readySnapshot();
    const market = localService(snapshot, "market-data");
    market.state = "stopped";
    market.desired_running = false;
    market.health = "offline";
    snapshot.market = null;
    return snapshot;
  });
  start();
  const notices = await screen.findByRole("list", { name: "进入前请留意" });
  expect(notices.textContent).toContain("本机行情服务按已保存意图保持停止");
  expect(sent(calls, "market.local", "node.action")).toEqual([]);
  expect(sent(calls, "node.data.local.open", "node.research.local.open")).toEqual([
    "node.data.local.open",
    "node.research.local.open",
  ]);
  fireEvent.click(screen.getByRole("button", { name: "进入工作台" }));
  expect(workbench()).not.toBeNull();
});

test("restoring account windows connects independent trading services without starting processes", async () => {
  const calls = serveCore(() => {
    const snapshot = readySnapshot();
    snapshot.trading_connections = ["local-stopped", "local-running", "remote-stopped"].map(id => ({
      id,
      name: id,
      host: "127.0.0.1",
      port: 7460,
      instance: id,
      fingerprint: "a".repeat(64),
      revision: "1",
    }));
    return snapshot;
  });
  start();
  await waitFor(() => expect(workbench()).not.toBeNull());
  expect(
    calls.filter(call => call.method === "live.open").map(call => call.params.account),
  ).toEqual(["local-stopped", "local-running", "remote-stopped"]);
  expect(sent(calls, "node.action")).toEqual([]);
});
