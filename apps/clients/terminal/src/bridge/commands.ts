// The current UI-to-Native contract. JSON is encoded only by the bridge.
// Command names and parameters stay correlated through host and component callbacks.

type InstrumentKey = { exchange: string; symbol: string };
import type { JsonValue } from "@bufbuild/protobuf";

export type CommandParams = {
  "runtime.snapshot": Record<string, never>;
  "runtime.python.status": Record<string, never>;
  "runtime.python.install": Record<string, never>;
  "trading.connections.save": {
    id: string;
    host: string;
    port: number;
    instance: string;
    fingerprint: string;
    revision: string;
  };
  "trading.connections.remove": { id: string; revision: string };
  "quotes.endpoint.save": { host: string; port: number; fingerprint: string };
  "quotes.endpoint.clear": Record<string, never>;
  "history.endpoint.save": { host: string; port: number; fingerprint: string };
  "history.endpoint.clear": Record<string, never>;
  "lab.endpoint": Record<string, never>;
  "quotes.open": Record<string, never>;
  "quotes.close": Record<string, never>;
  "quotes.watchlist": { instruments: InstrumentKey[] };
  "quotes.watch": { restart: boolean };
  "trading.call": { account: string; request: JsonValue };
  "quotes.call": { request: JsonValue };
  "history.call": { request: JsonValue };
  "node.action": { service: string; action: "start" | "stop" | "restart" };
};
export type TerminalCommand = Exclude<keyof CommandParams, "runtime.snapshot">;
export type CommandCall<K extends keyof CommandParams = TerminalCommand> = {
  [M in K]: Record<string, never> extends CommandParams[M]
    ? [method: M, params?: CommandParams[M]]
    : [method: M, params: CommandParams[M]];
}[K];
export type CommandHandler<R = void, K extends keyof CommandParams = TerminalCommand> = (
  ...args: CommandCall<K>
) => Promise<R>;
