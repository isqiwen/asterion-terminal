import type { MessageInitShape } from "@bufbuild/protobuf";
import type { RequestSchema } from "../generated/asterion/strategy_pb";
import { tradingCall } from "./trading";

// Mutations are never retried; read the host again after an unknown outcome.
export function strategyRequest(
  account: string,
  operation: MessageInitShape<typeof RequestSchema>["operation"],
) {
  return tradingCall(account, { case: "strategy", value: { operation } }, "strategy");
}
