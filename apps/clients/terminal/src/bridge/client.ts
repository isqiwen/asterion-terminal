import { BackendError } from "../i18n/errors";
import { translate, type MessageValues, getLocale } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { desktop } from "./desktop";

import { nativeDesktop } from "./desktop";
export type FuturesContract = {
  venue: string;
  symbol: string;
  product: string;
  delivery_month: string;
  currency: string;
  price_increment: string;
  quantity_increment: string;
  multiplier: string;
};
export type { TerminalCommand, CommandParams, CommandCall, CommandHandler } from "./commands";
import type { TerminalCommand, CommandCall } from "./commands";
export type ProgressObservation = {
  observed: boolean;
  pending: boolean;
  age_ms: number;
};
export type ExecutionHealth = {
  io: ProgressObservation;
  state: ProgressObservation;
  persistence: ProgressObservation;
  initialization: ProgressObservation;
  command: ProgressObservation;
  business_ready: boolean;
};
export type NodeStatus = {
  id: string;
  host: string;
  port: number;
  state: "online" | "unreachable";
  last_heartbeat_ms: number;
  latency_ms: number;
  error: string;
  health: null | {
    instance_id: string;
    phase: "initializing" | "ready" | "recovery_required";
    failure: null | { code: string; message: string };
    execution: ExecutionHealth;
    pid: number;
    os: string;
    arch: string;
    version: string;
    uptime_ms: number;
    services: {
      kind: "lab";
      id: string;
      state: string;
      desired_running: boolean;
      pid: number;
      restarts: number;
      error: string;
      health: string;
      execution: ExecutionHealth | null;
      last_heartbeat_ms: number;
      endpoint: string;
      directory: string;
    }[];
  };
};
export type UnreadableCtpConnection = { id: string; name: string; error: "unreadable" };
export type TradingConnection = {
  id: string;
  name: string;
  host: string;
  port: number;
  instance: string;
  fingerprint: string;
  revision: string;
};

// The Python runtime the first launch installs: each module's bytes so far.
export type PythonRuntime = {
  state: "missing" | "installing" | "ready" | "failed";
  error: string;
  modules: {
    id: string;
    title: string;
    size: number;
    done: number;
    state: "waiting" | "downloading" | "downloaded" | "installing" | "done" | "failed";
  }[];
};
export type Snapshot = {
  trading_connections?: (TradingConnection | UnreadableCtpConnection)[];
  trading_fingerprint?: string;
  // The saved Market host, or a mark that its settings file is unreadable.
  market_endpoint?:
    | null
    | {
        host: string;
        port: number;
        fingerprint: string;
        instruments: { venue: string; symbol: string }[];
      }
    | { error: "unreadable" };
  // The fixed strategy directory, when the entry point names one.
  research_directory?: string | null;
  python_runtime?: PythonRuntime;
  // Revision of the core's published state and when the core last refreshed it.
  revision: number;
  refreshed_at_ms: number;
  nodes: NodeStatus[];
  protocol: 1;
  product: string;
  core: string;
  phase: "ready";
  asset: "futures";
  // The core's rule per exchange for assigning closes to today's and
  // yesterday's positions.
  close_policies: Record<string, "explicit_buckets" | "today_first" | "yesterday_first">;
  diagnostics: {
    log_directory: string;
    succeeded: number;
    failed: number;
    log_failures: number;
    refresh_failures: number;
    refresh_failed: boolean;
  };
};
// One native command; its reply as the core sent it.
export async function call(
  method: "runtime.snapshot" | TerminalCommand,
  params: Record<string, unknown>,
): Promise<unknown> {
  const body = JSON.stringify({ version: 1, method, params });
  let raw: string;
  if (nativeDesktop) raw = await desktop().request(body);
  else if (import.meta.env.DEV) {
    const response = await fetch("/__asterion/api", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body,
    });
    if (!response.ok) throw new Error(t("本机 C++ 服务不可用，请重新启动终端"));
    raw = await response.text();
  } else throw new Error(t("请在 Asterion Terminal 桌面应用中打开"));
  const envelope = JSON.parse(raw);
  if (envelope.error)
    throw new BackendError(envelope.error.code ?? "operation_failed", envelope.error.message);
  return envelope.result;
}
function snapshotContract(result: unknown): Snapshot {
  const value = result as Snapshot;
  if (!value || value.protocol !== 1 || value.phase !== "ready" || value.asset !== "futures") {
    throw new Error(t("本机核心返回了不支持的状态契约"));
  }
  return value;
}
export async function request(
  ...args: CommandCall<keyof import("./commands").CommandParams>
): Promise<Snapshot> {
  return snapshotContract(await call(args[0], args[1] ?? {}));
}
export function timestamp(ns: string | null): string {
  if (!ns) return "—";
  const date = new Date(Number(BigInt(ns) / 1000000n));
  return date.toLocaleString(getLocale(), { hour12: false, timeZone: "Asia/Shanghai" });
}
// Polls send the last revision they saw; an unchanged core returns no state.
export type SnapshotUnchanged = { unchanged: true; revision: number; refreshed_at_ms: number };
export async function pollSnapshot(since: number): Promise<Snapshot | SnapshotUnchanged> {
  const result = (await call("runtime.snapshot", { since })) as Partial<SnapshotUnchanged>;
  if (result?.unchanged === true) {
    if (typeof result.revision !== "number" || typeof result.refreshed_at_ms !== "number")
      throw new Error(t("本机核心返回了不支持的状态契约"));
    return result as SnapshotUnchanged;
  }
  return snapshotContract(result);
}
