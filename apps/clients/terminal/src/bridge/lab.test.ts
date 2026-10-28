import { create, fromBinary, toBinary } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import { RequestSchema, ResponseSchema } from "../generated/asterion/lab_pb";
import { BackendError } from "../i18n/errors";
import { lab } from "./lab";

// The development server carries the bytes; answer as the lab service would.
function serve(answer: (request: ReturnType<typeof decode>) => Uint8Array) {
  vi.stubGlobal(
    "fetch",
    vi.fn(async (_url: string, init: RequestInit) => {
      const request = decode(new Uint8Array(init.body as ArrayBuffer));
      return new Response(answer(request).slice().buffer, { status: 200 });
    }),
  );
}
const decode = (bytes: Uint8Array) => fromBinary(RequestSchema, bytes);
afterEach(() => vi.unstubAllGlobals());

test("a request is encoded with the generated types and its reply decoded", async () => {
  serve(request => {
    expect(request).toMatchObject({ version: 1, serviceId: "lab", body: { case: "tasks" } });
    return toBinary(
      ResponseSchema,
      create(ResponseSchema, {
        version: 1,
        serviceId: "lab",
        correlationId: request.correlationId,
        body: { case: "tasks", value: { tasks: [{ id: "daily-1", state: "running" }] } },
      }),
    );
  });
  const tasks = await lab({ case: "tasks", value: {} }, "tasks");
  expect(tasks.tasks.map(task => task.id)).toEqual(["daily-1"]);
});

test("a service error and a reply to another request are errors", async () => {
  serve(request =>
    toBinary(
      ResponseSchema,
      create(ResponseSchema, {
        version: 1,
        serviceId: "lab",
        correlationId: request.correlationId,
        body: { case: "error", value: { code: "invalid_request", message: "credential first" } },
      }),
    ),
  );
  await expect(lab({ case: "summary", value: {} }, "summary")).rejects.toMatchObject({
    code: "invalid_request",
  });
  serve(() =>
    toBinary(
      ResponseSchema,
      create(ResponseSchema, { version: 1, serviceId: "lab", correlationId: "another" }),
    ),
  );
  await expect(lab({ case: "summary", value: {} }, "summary")).rejects.toBeInstanceOf(BackendError);
});
