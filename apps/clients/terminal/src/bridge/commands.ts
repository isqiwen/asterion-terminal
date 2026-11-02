// The current UI-to-Native contract. JSON is encoded only by the bridge.
// Command names and parameters stay correlated through host and component callbacks.

type InstrumentKey = { venue: string; symbol: string };
import type { JsonValue } from "@bufbuild/protobuf";
type RiskParameters = {
  max_order_quantity: string;
  max_gross_quantity: string;
  max_working_orders: string;
};
type AccountCommand = {
  account: string;
  request_id: string;
  account_id: string;
  policy_revision: string;
};
export type LiveAction =
  | { action: "live_authorize"; user_id: string }
  | { action: "live_revoke" }
  | { action: "live_resolve"; order_id: string }
  | { action: "cancel"; order_id: string }
  | {
      action: "submit";
      order_id: string;
      venue: string;
      symbol: string;
      side: "buy" | "sell";
      offset: "open" | "close_today" | "close_yesterday" | "close";
      quantity: string;
      price: string;
    };

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
  "research.repository.save": { path: string };
  "research.repository.clear": Record<string, never>;
  "lab.endpoint": Record<string, never>;
  "market.open": Record<string, never>;
  "market.close": Record<string, never>;
  "market.watchlist": { instruments: InstrumentKey[] };
  "market.minutes": { venue: string; symbol: string };
  "market.recording": Record<string, never>;
  "trading.call": { account: string; request: JsonValue };
  "market.data": { request: JsonValue };
  "live.create": RiskParameters & {
    account: string;
    max_price_deviation: string;
    contracts: InstrumentKey[];
  };
  "live.policy.configure": RiskParameters &
    AccountCommand & {
      max_price_deviation: string;
      contracts: InstrumentKey[];
    };
  "live.open": { account: string };
  "live.connect": { account: string };
  "live.disconnect": { account: string };
  "live.costs": { account: string };
  "live.act": AccountCommand & LiveAction;
  "live.close": { account: string };
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

// A trading panel is bound to one connection; its adapter supplies that account.
export type AccountMethod = Extract<TerminalCommand, `live.${string}`>;
type WithoutAccount<P> = P extends { account: string } ? Omit<P, "account"> : never;
export type AccountCall = {
  [M in AccountMethod]: Record<string, never> extends WithoutAccount<CommandParams[M]>
    ? [method: M, params?: Record<string, never>]
    : [method: M, params: WithoutAccount<CommandParams[M]>];
}[AccountMethod];
export type AccountRun = (...args: AccountCall) => Promise<boolean>;
