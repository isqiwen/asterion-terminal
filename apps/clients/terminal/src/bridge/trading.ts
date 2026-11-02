// The generated trading.proto contract over the account's saved pinned TLS
// endpoint; Native owns the connection and relays the ProtoJSON unchanged.
// Commands are never retried: after an unknown outcome, read the account again.
import {
  create,
  fromJson,
  toJson,
  type JsonValue,
  type MessageInitShape,
} from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/trading_pb";
import { BackendError } from "../i18n/errors";
import { call } from "./client";

export type TradingOperation = MessageInitShape<typeof RequestSchema>["operation"];
export type TradingFields = Pick<
  MessageInitShape<typeof RequestSchema>,
  "accountId" | "policyRevision"
>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

export async function tradingRequest(
  account: string,
  operation: TradingOperation,
  fields: TradingFields = {},
): Promise<Response> {
  const reply = (await call("trading.call", {
    account,
    request: toJson(RequestSchema, create(RequestSchema, { ...fields, operation })),
  })) as { trading?: JsonValue };
  if (!reply.trading) throw new BackendError("operation_failed", "Missing trading response");
  return fromJson(ResponseSchema, reply.trading);
}

export async function tradingCall<C extends NonNullable<Result["case"]>>(
  account: string,
  operation: TradingOperation,
  expected: C,
): Promise<Reply<C>> {
  const response = await tradingRequest(account, operation);
  if (response.result.case !== expected)
    throw new BackendError("operation_failed", "Trading replied with another operation");
  return response.result.value as Reply<C>;
}
