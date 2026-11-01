import { create, fromJson, toJson, type MessageInitShape } from "@bufbuild/protobuf";
import { RequestSchema, StateSchema } from "../generated/asterion/strategy_pb";
import { request } from "./client";
import { BackendError } from "../i18n/errors";

// Generated contract over the account's saved pinned TLS endpoint. Mutations
// are never retried; reconnect and inspect the host after an unknown outcome.
export async function strategyRequest(
  account: string,
  operation: MessageInitShape<typeof RequestSchema>["operation"],
) {
  const reply = await request("trading.strategy", {
    account,
    request: toJson(RequestSchema, create(RequestSchema, { operation })),
  });
  if (!reply.strategy) throw new BackendError("operation_failed", "Missing strategy response");
  return fromJson(StateSchema, reply.strategy);
}
