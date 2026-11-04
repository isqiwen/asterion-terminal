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
// Lab as the Terminal runs it; it restarts by itself until it runs again.
export type LabStatus = {
  state: "starting" | "ready" | "unresponsive" | "restarting";
  pid: number;
  restarts: number;
  error: string;
};
export type UnreadableCtpConnection = { id: string; error: "unreadable" };
export type TradingConnection = {
  id: string;
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
  client_fingerprint?: string;
  // The saved Market host, or a mark that its settings file is unreadable.
  market_endpoint?:
    | null
    | {
        host: string;
        port: number;
        fingerprint: string;
        instruments: { exchange: string; symbol: string }[];
      }
    | { error: "unreadable" };
  // The fixed strategy directory, when the entry point names one.
  research_directory?: string | null;
  python_runtime?: PythonRuntime;
  // Revision of the core's published state and when the core last refreshed it.
  revision: number;
  refreshed_at_ms: number;
  // Null until Lab was first started.
  lab: LabStatus | null;
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
