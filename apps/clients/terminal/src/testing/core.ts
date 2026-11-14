import { vi } from "vitest";
import type { Snapshot } from "../bridge/client";

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

// What the core reports, as of now, for a local installation with Market set
// and Lab up and nothing else: no accounts or quotes. A test changes
// the parts its case is about.
export function readySnapshot(): Snapshot {
  return {
    revision: 1,
    refreshed_at_ms: Date.now(),
    warehouse_endpoint: {
      host: "research",
      port: 7450,
      fingerprint: "a".repeat(64),
      instruments: [],
    },
    market_endpoint: { host: "research", port: 7451, fingerprint: "a".repeat(64), instruments: [] },
    lab: { state: "ready", pid: 1, restarts: 0, error: "" },
    protocol: 1,
    product: "Asterion Terminal",
    core: "test",
    phase: "ready",
    asset: "futures",
    close_policies: {},
    diagnostics: {
      version: "test",
      commit: "test",
      log_directory: "/test-node/logs",
      succeeded: 0,
      failed: 0,
      log_failures: 0,
      refresh_failures: 0,
      refresh_failed: false,
    },
  };
}
