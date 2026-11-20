// The generated market.proto contract over the saved service's pinned TLS
// endpoint; Native owns the connection and relays the ProtoJSON unchanged.
import type { MessageInitShape } from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/market_pb";
import { BackendError } from "../i18n/errors";
import { call } from "./client";
import { protoRequest } from "./proto-call";

type Call = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

// The result of one call: Market answers each call in the field of the same name.
export async function marketData<C extends NonNullable<Call["case"]>>(
  operation: Extract<Call, { case: C }>,
): Promise<Reply<C>> {
  const response = await protoRequest(
    request => call("market.call", { request }),
    RequestSchema,
    ResponseSchema,
    { call: operation },
    "market",
    "Missing Market response",
  );
  if (response.result.case !== (operation as { case: C }).case)
    throw new BackendError("operation_failed", "Market replied with another operation");
  return response.result.value as Reply<C>;
}
