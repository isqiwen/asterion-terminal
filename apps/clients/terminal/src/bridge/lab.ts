// Requests to the Python lab service, built and read with the types generated
// from protocol/proto/asterion/lab.proto. The Electron main process (or the
// development server) carries the bytes; it owns the connection.
import { create, fromBinary, toBinary, type MessageInitShape } from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/lab_pb";
import { BackendError } from "../i18n/errors";
import { desktop, nativeDesktop } from "./desktop";

type RequestBody = MessageInitShape<typeof RequestSchema>["body"];
type ReplyBody = Response["body"];
type Reply<C> = Extract<ReplyBody, { case: C }> extends { value: infer V } ? V : never;
let sequence = 0;

async function carry(payload: Uint8Array): Promise<Uint8Array> {
  if (nativeDesktop) return await desktop().lab(payload);
  if (import.meta.env.DEV) {
    const response = await fetch("/__asterion/lab", {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: payload.slice().buffer,
    });
    if (!response.ok) throw new BackendError("unavailable", await response.text());
    return new Uint8Array(await response.arrayBuffer());
  }
  throw new BackendError("unavailable", "Lab service requires the desktop application");
}

// The reply of one request, or a BackendError with the service's code.
export async function lab<C extends NonNullable<ReplyBody["case"]>>(
  body: RequestBody,
  expected: C,
): Promise<Reply<C>> {
  const correlationId = `ui.${Date.now()}.${++sequence}`;
  const request = create(RequestSchema, { version: 1, serviceId: "lab", correlationId, body });
  const reply = fromBinary(ResponseSchema, await carry(toBinary(RequestSchema, request)));
  if (reply.correlationId !== correlationId)
    throw new BackendError("operation_failed", "Lab service reply identity mismatch");
  if (reply.body.case === "error")
    throw new BackendError(reply.body.value.code, reply.body.value.message);
  if (reply.body.case !== expected)
    throw new BackendError("operation_failed", "Lab service replied with another operation");
  return reply.body.value as Reply<C>;
}
