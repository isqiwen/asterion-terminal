// The generated warehouse.proto contract over the saved service's pinned TLS
// endpoint; Native owns the connection and relays the ProtoJSON unchanged.
// Writes are never retried.
import type { MessageInitShape } from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/warehouse_pb";
import { BackendError } from "../i18n/errors";
import { call } from "./client";
import { protoRequest } from "./proto-call";

type Call = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

// The result of one call: Warehouse answers each call in the field of the same name.
export async function warehouseData<C extends NonNullable<Call["case"]>>(
  operation: Extract<Call, { case: C }>,
): Promise<Reply<C>> {
  const response = await protoRequest(
    request => call("warehouse.call", { request }),
    RequestSchema,
    ResponseSchema,
    { call: operation },
    "warehouse",
    "Missing Warehouse response",
  );
  if (response.result.case !== (operation as { case: C }).case)
    throw new BackendError("operation_failed", "Warehouse replied with another operation");
  return response.result.value as Reply<C>;
}
