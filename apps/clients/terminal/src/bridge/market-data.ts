// Native owns the saved endpoint, identity and pinned TLS connection. Only
// generated Protobuf JSON crosses the existing desktop request boundary.
import { create, fromJson, toJson, type MessageInitShape } from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/market_pb";
import { BackendError } from "../i18n/errors";
import { request } from "./client";

type Operation = MessageInitShape<typeof RequestSchema>["operation"];
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;
let sequence = 0;

export async function marketData<C extends NonNullable<Result["case"]>>(
  operation: Operation,
  expected: C,
): Promise<Reply<C>> {
  const correlationId = `data.${Date.now()}.${++sequence}`;
  const message = create(RequestSchema, {
    version: 1,
    serviceId: "market-data",
    correlationId,
    operation,
  });
  const snapshot = await request("market.data", { request: toJson(RequestSchema, message) });
  if (!snapshot.market_data)
    throw new BackendError("operation_failed", "Missing Market data response");
  const response = fromJson(ResponseSchema, snapshot.market_data);
  if (
    response.version !== 1 ||
    response.serviceId !== message.serviceId ||
    response.correlationId !== correlationId
  )
    throw new BackendError("operation_failed", "Market data response identity mismatch");
  if (response.result.case === "error")
    throw new BackendError(response.result.value.code, response.result.value.message);
  if (response.result.case !== expected)
    throw new BackendError("operation_failed", "Market replied with another operation");
  return response.result.value as Reply<C>;
}
