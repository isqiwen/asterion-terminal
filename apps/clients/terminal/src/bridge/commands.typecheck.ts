import type { CommandHandler } from "./commands";
import type { TerminalContext } from "../../features/contract";

// Compiled by the normal TypeScript build; never executed or bundled into the app.
export function commandContractExamples(send: CommandHandler, context: TerminalContext) {
  void send("market.open");
  void send("node.action", { service: "lab", action: "restart" });
  void context.query("market.minutes", { venue: "SHFE", symbol: "rb2605" });
  // @ts-expect-error a trading request names its account
  void send("trading.call", { request: {} });
  void send("node.action", {
    service: "lab",
    // @ts-expect-error parameters belong to their command
    account: "account",
  });
  // @ts-expect-error a query callback cannot reach a trading account
  void context.query("trading.call", { account: "account", request: {} });
  // @ts-expect-error no parameters are accepted by the runtime status command
  void send("runtime.python.status", { path: "runtime" });
}
