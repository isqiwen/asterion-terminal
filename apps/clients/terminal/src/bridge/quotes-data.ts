// The generated quotes.proto contract over the saved service's pinned TLS
// endpoint; Native owns the connection and relays the ProtoJSON unchanged.
import {
  create,
  fromJson,
  toJson,
  type JsonValue,
  type MessageInitShape,
} from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/quotes_pb";
import { BackendError } from "../i18n/errors";
import { call } from "./client";

type Call = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

// The result of one call: Quotes answers each call in the field of the same name.
export async function quotesData<C extends NonNullable<Call["case"]>>(
  operation: Extract<Call, { case: C }>,
): Promise<Reply<C>> {
  const reply = (await call("quotes.call", {
    request: toJson(RequestSchema, create(RequestSchema, { call: operation })),
  })) as { quotes?: JsonValue };
  if (!reply.quotes) throw new BackendError("operation_failed", "Missing Quotes response");
  const response = fromJson(ResponseSchema, reply.quotes);
  if (response.result.case !== (operation as { case: C }).case)
    throw new BackendError("operation_failed", "Quotes replied with another operation");
  return response.result.value as Reply<C>;
}
