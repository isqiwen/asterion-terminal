import type { AccountRun, CommandHandler } from "./commands";
import type { TerminalContext } from "../../features/contract";

// Compiled by the normal TypeScript build; never executed or bundled into the app.
export function commandContractExamples(
  send: CommandHandler,
  context: TerminalContext,
  account: AccountRun,
) {
  void send("market.local");
  void send("node.action", { service: "market-data", action: "restart" });
  void context.query("market.minutes", { venue: "SHFE", symbol: "rb2605" });
  void account("live.open");
  // @ts-expect-error a panel-bound command cannot supply a different account
  void account("live.open", { account: "other" });
  // @ts-expect-error submit requires its complete order, not only authorization identity
  void send("live.act", {
    account: "a",
    request_id: "r",
    account_id: "id",
    policy_revision: "p",
    action: "submit",
  });
  // @ts-expect-error a required account cannot disappear through a host callback
  void send("live.open");
  void send("node.action", {
    service: "market-data",
    // @ts-expect-error parameters belong to their command
    account: "account",
  });
  // @ts-expect-error a query callback cannot send an order or change authorization
  void context.query("live.open", { account: "account" });
  // @ts-expect-error no parameters are accepted by the runtime status command
  void send("runtime.python.status", { path: "runtime" });
}
