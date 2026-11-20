// Shared ProtoJSON call framing over the native bridge.
import {
  create,
  fromJson,
  toJson,
  type DescMessage,
  type JsonValue,
  type MessageInitShape,
  type MessageShape,
} from "@bufbuild/protobuf";
import { BackendError } from "../i18n/errors";

export async function protoRequest<Req extends DescMessage, Res extends DescMessage>(
  invoke: (request: JsonValue) => Promise<unknown>,
  requestSchema: Req,
  responseSchema: Res,
  init: MessageInitShape<Req>,
  replyField: string,
  missingMessage: string,
): Promise<MessageShape<Res>> {
  const reply = (await invoke(toJson(requestSchema, create(requestSchema, init)))) as Record<
    string,
    JsonValue | undefined
  >;
  const raw = reply[replyField];
  if (!raw) throw new BackendError("operation_failed", missingMessage);
  try {
    return fromJson(responseSchema, raw);
  } catch {
    throw new BackendError("conflict", `${replyField} protocol differs from this Terminal`);
  }
}
