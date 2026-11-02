// The generated trading.proto contract over the account's saved pinned TLS
// endpoint; Native owns the connection and relays the ProtoJSON unchanged.
// Commands are never retried: after an unknown outcome, read the account again.
import { create, fromJson, toJson, type MessageInitShape } from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/trading_pb";
import { BackendError } from "../i18n/errors";
import { request } from "./client";

type Operation = MessageInitShape<typeof RequestSchema>["operation"];
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

export async function tradingCall<C extends NonNullable<Result["case"]>>(
  account: string,
  operation: Operation,
  expected: C,
  fields: Pick<MessageInitShape<typeof RequestSchema>, "accountId" | "policyRevision"> = {},
): Promise<Reply<C>> {
  const reply = await request("trading.call", {
    account,
    request: toJson(RequestSchema, create(RequestSchema, { ...fields, operation })),
  });
  if (!reply.trading) throw new BackendError("operation_failed", "Missing trading response");
  const response = fromJson(ResponseSchema, reply.trading);
  if (response.result.case !== expected)
    throw new BackendError("operation_failed", "Trading replied with another operation");
  return response.result.value as Reply<C>;
}
