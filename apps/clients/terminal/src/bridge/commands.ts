// The current UI-to-Native contract. JSON is encoded only by the bridge.
// Command names and parameters stay correlated through host and component callbacks.

type InstrumentKey = { venue: string; symbol: string };
import type { JsonValue } from "@bufbuild/protobuf";

export type CommandParams = {
  "runtime.snapshot": Record<string, never>;
  "runtime.python.status": Record<string, never>;
  "runtime.python.install": Record<string, never>;
  "trading.identity.prepare": Record<string, never>;
  "trading.connections.save": {
    id: string;
    name: string;
    host: string;
    port: number;
    instance: string;
    fingerprint: string;
    revision: string;
  };
  "trading.connections.remove": { id: string; revision: string };
  "market.endpoint.save": { host: string; port: number; fingerprint: string };
  "market.endpoint.clear": Record<string, never>;
  "lab.endpoint": Record<string, never>;
  "market.open": Record<string, never>;
  "market.close": Record<string, never>;
  "market.watchlist": { instruments: InstrumentKey[] };
  "market.watch": { restart: boolean };
  "trading.call": { account: string; request: JsonValue };
  "market.call": { request: JsonValue };
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
