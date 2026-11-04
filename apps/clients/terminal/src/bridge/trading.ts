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

export type TradingCall = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

export async function tradingRequest(account: string, operation: TradingCall): Promise<Response> {
  const reply = (await call("trading.call", {
    account,
    request: toJson(RequestSchema, create(RequestSchema, { call: operation })),
  })) as { trading?: JsonValue };
  if (!reply.trading) throw new BackendError("operation_failed", "Missing trading response");
  return fromJson(ResponseSchema, reply.trading);
}

// The result of one call: Trading answers each call in the field of the same name.
export async function tradingCall<C extends NonNullable<TradingCall["case"]>>(
  account: string,
  operation: Extract<TradingCall, { case: C }>,
): Promise<Reply<C>> {
  const response = await tradingRequest(account, operation);
  if (response.result.case !== (operation as { case: C }).case)
    throw new BackendError("operation_failed", "Trading replied with another operation");
  return response.result.value as Reply<C>;
}
