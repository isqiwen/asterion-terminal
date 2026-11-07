// The generated warehouse.proto contract over the saved service's pinned TLS
// endpoint; Native owns the connection and relays the ProtoJSON unchanged.
// Writes are never retried.
import {
  create,
  fromJson,
  toJson,
  type JsonValue,
  type MessageInitShape,
} from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/warehouse_pb";
import { BackendError } from "../i18n/errors";
import { call } from "./client";

type Call = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;

// The result of one call: Warehouse answers each call in the field of the same name.
export async function warehouseData<C extends NonNullable<Call["case"]>>(
  operation: Extract<Call, { case: C }>,
): Promise<Reply<C>> {
  const reply = (await call("warehouse.call", {
    request: toJson(RequestSchema, create(RequestSchema, { call: operation })),
  })) as { warehouse?: JsonValue };
  if (!reply.warehouse) throw new BackendError("operation_failed", "Missing Warehouse response");
  const response = fromJson(ResponseSchema, reply.warehouse);
  if (response.result.case !== (operation as { case: C }).case)
    throw new BackendError("operation_failed", "Warehouse replied with another operation");
  return response.result.value as Reply<C>;
}
