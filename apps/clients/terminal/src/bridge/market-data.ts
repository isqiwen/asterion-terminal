// The generated market.proto contract over the saved Market's pinned TLS
// endpoint; Native owns the connection, sets the request identity and relays
// the ProtoJSON unchanged. Writes are never retried.
import {
  create,
  fromJson,
  toJson,
  type JsonValue,
  type MessageInitShape,
} from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/history_pb";
import { BackendError } from "../i18n/errors";
import { call } from "./client";

type Call = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

// The result of one call: Market answers each call in the field of the same name.
export async function marketData<C extends NonNullable<Call["case"]>>(
  operation: Extract<Call, { case: C }>,
): Promise<Reply<C>> {
  const reply = (await call("market.call", {
    request: toJson(RequestSchema, create(RequestSchema, { call: operation })),
  })) as { market?: JsonValue };
  if (!reply.market) throw new BackendError("operation_failed", "Missing Market response");
  const response = fromJson(ResponseSchema, reply.market);
  if (response.result.case !== (operation as { case: C }).case)
    throw new BackendError("operation_failed", "Market replied with another operation");
  return response.result.value as Reply<C>;
}
