import type { CommandHandler } from "./commands";

// Compiled by the normal TypeScript build; never executed or bundled into the app.
export function commandContractExamples(send: CommandHandler) {
  void send("market.open");
  void send("node.action", { service: "lab", action: "restart" });
  // @ts-expect-error a trading request names its account
  void send("trading.call", { request: {} });
  void send("node.action", {
    service: "lab",
    // @ts-expect-error parameters belong to their command
    account: "account",
  });
  // @ts-expect-error no parameters are accepted by the runtime status command
  void send("runtime.python.status", { path: "runtime" });
}
