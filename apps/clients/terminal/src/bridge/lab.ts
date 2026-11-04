// Requests to the Python lab service, built and read with the types generated
// from protocol/proto/asterion/lab.proto. The Electron main process (or the
// development server) carries the bytes; it owns the connection.
import { create, fromBinary, toBinary, type MessageInitShape } from "@bufbuild/protobuf";
import { RequestSchema, ResponseSchema, type Response } from "../generated/asterion/lab_pb";
import { BackendError, remoteError } from "../i18n/errors";
import { desktop, nativeDesktop } from "./desktop";

type Call = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type Result = Response["result"];
type Reply<C> = Extract<Result, { case: C }> extends { value: infer V } ? V : never;
let sequence = 0n;

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

// The result of one call: Lab answers each call in the field of the same name,
// or with an error, raised as a BackendError with its code.
export async function lab<C extends NonNullable<Call["case"]>>(
  operation: Extract<Call, { case: C }>,
): Promise<Reply<C>> {
  const id = ++sequence;
  const request = create(RequestSchema, { id, call: operation });
  const reply = fromBinary(ResponseSchema, await carry(toBinary(RequestSchema, request)));
  if (reply.id !== id)
    throw new BackendError("operation_failed", "Lab service reply identity mismatch");
  if (reply.result.case === "error") throw remoteError(reply.result.value);
  if (reply.result.case !== (operation as { case: C }).case)
    throw new BackendError("operation_failed", "Lab service replied with another operation");
  return reply.result.value as Reply<C>;
}
