import { vi } from "vitest";
import type { ExecutionHealth, NodeStatus, Snapshot } from "../bridge/client";

// Component tests replace the C++ core at the one place the UI reaches it,
// the development transport. The test decides every reply; nothing here
// imitates what the core would do.
export type CoreReply = Snapshot | { error: { code?: string; message: string } };
export type CoreCall = { method: string; params: Record<string, unknown> };

// Answers the UI's commands with `reply` and returns the commands it sent.
export function serveCore(reply: (call: CoreCall) => CoreReply): CoreCall[] {
  const calls: CoreCall[] = [];
  vi.stubGlobal("fetch", async (_url: string, init: { body: string }) => {
    const { method, params } = JSON.parse(init.body) as CoreCall;
    calls.push({ method, params });
    const answer = reply({ method, params });
    const body = JSON.stringify("error" in answer ? answer : { result: answer });
    return { ok: true, text: async () => body };
  });
  return calls;
}

const settled = { observed: true, pending: false, age_ms: 0 };
const execution: ExecutionHealth = {
  io: settled,
  state: settled,
  persistence: settled,
  initialization: settled,
  command: settled,
  business_ready: true,
};
type Service = NonNullable<NodeStatus["health"]>["services"][number];
const service = (id: string, kind: Service["kind"]): Service => ({
  revision: "1",
  kind,
  task_service: kind === "data" ? "task" : "",
  data_service: kind === "task" ? "historical-data" : "",
  active_workers: 0,
  resource_request: { cpu_slots: 1, memory_mib: 256, io_slots: 1 },
  plugin_artifacts: [],
  id,
  artifact: "a".repeat(64),
  port: 0,
  state: "running",
  desired_running: true,
  pid: 1,
  restarts: 0,
  error: "",
  health: "ready",
  execution,
  last_heartbeat_ms: Date.now(),
  endpoint: "",
  directory: "",
});
const connection = (service: string) => ({
  connection_id: "local",
  port: 0,
  service,
  host: "",
  remote: false,
  online: true,
  error: "",
});

// What the core reports, as of now, for a local installation with the Agent
// and its market, data and task services up and nothing else: no accounts,
// datasets, tasks or quotes. A test changes the parts its case is about.
export function readySnapshot(): Snapshot {
  return {
    daily_page: null,
    history_page: null,
    history_contracts: { source: "", exchange: "", product: "", cutoff_ns: "0", items: [] },
    revision: 1,
    refreshed_at_ms: Date.now(),
    task_service: {
      ...connection("task"),
      tasks: [],
      before_sequence: 0,
      next_before_sequence: 0,
      failed_count: 0,
      interrupted_count: 0,
      capacity: null,
    },
    data: { ...connection("historical-data"), sources: [] },
    task_result: null,
    market: {
      watchlist: [],
      catalog: { phase: "idle", error_code: "", diagnostic: "", trading_day: "", contracts: [] },
      history: { stream_id: "", generation: "", available: false, interrupted: false, points: [] },
      service: "market-data",
      remote: false,
      host: "",
      port: 0,
      instance_id: "market",
      phase: "awaiting_input",
      error_code: 0,
      transport_online: true,
      sequence: 0,
      out_of_order: 0,
      subscriptions: [],
    },
    ssh_key: null,
    agent_program: {
      state: "current",
      expected_digest: "a".repeat(64),
      installed_digest: "a".repeat(64),
      bundled_digest: "a".repeat(64),
    },
    firewall_plan: null,
    nodes: [
      {
        id: "local",
        host: "",
        port: 0,
        state: "online",
        last_heartbeat_ms: Date.now(),
        latency_ms: 1,
        error: "",
        health: {
          instance_id: "agent",
          phase: "ready",
          failure: null,
          execution,
          worker_capacity: { limit: 4, owned: 0, reserved: 0 },
          resource_budget: null,
          maintenance: false,
          pid: 1,
          os: "linux",
          arch: "x86_64",
          version: "test",
          uptime_ms: 1,
          services: [
            service("market-data", "market"),
            service("historical-data", "data"),
            service("task", "task"),
          ],
        },
      },
    ],
    protocol: 1,
    product: "Asterion Terminal",
    core: "test",
    phase: "ready",
    asset: "futures",
    close_policies: {},
    datasets: [],
    dataset_series: [],
    native_plugins: { directory: "", items: [] },
    plugins: [],
    diagnostics: {
      succeeded: 0,
      failed: 0,
      log_failures: 0,
      refresh_failures: 0,
      refresh_failed: false,
    },
    live: {},
  };
}

// The local node's health and one of its services, for a test to change.
export function localHealth(snapshot: Snapshot) {
  return snapshot.nodes.find(node => node.id === "local")!.health!;
}
export function localService(snapshot: Snapshot, id: string) {
  return localHealth(snapshot).services.find(service => service.id === id)!;
}
