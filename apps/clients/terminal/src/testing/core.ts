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
  kind,
  id,
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
// What the core reports, as of now, for a local installation with its market
// and lab services up and nothing else: no accounts or quotes. A test changes
// the parts its case is about.
export function readySnapshot(): Snapshot {
  return {
    revision: 1,
    refreshed_at_ms: Date.now(),
    market_endpoint: { host: "research", port: 7450, fingerprint: "a".repeat(64), instruments: [] },
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
          instance_id: "terminal",
          phase: "ready",
          failure: null,
          execution,
          pid: 1,
          os: "linux",
          arch: "x86_64",
          version: "test",
          uptime_ms: 1,
          services: [service("lab", "lab")],
        },
      },
    ],
    protocol: 1,
    product: "Asterion Terminal",
    core: "test",
    phase: "ready",
    asset: "futures",
    close_policies: {},
    diagnostics: {
      log_directory: "/test-node/logs",
      succeeded: 0,
      failed: 0,
      log_failures: 0,
      refresh_failures: 0,
      refresh_failed: false,
    },
  };
}

// The local node's health and one of its services, for a test to change.
export function localHealth(snapshot: Snapshot) {
  return snapshot.nodes.find(node => node.id === "local")!.health!;
}
export function localService(snapshot: Snapshot, id: string) {
  return localHealth(snapshot).services.find(service => service.id === id)!;
}
