import { create, fromBinary, toBinary } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import { ErrorCode } from "../generated/asterion/common_pb";
import { RequestSchema, ResponseSchema, RunState } from "../generated/asterion/lab_pb";
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
    expect(request).toMatchObject({ call: { case: "listRuns" } });
    return toBinary(
      ResponseSchema,
      create(ResponseSchema, {
        id: request.id,
        result: {
          case: "listRuns",
          value: { runs: [{ id: "research-1", state: RunState.RUNNING }] },
        },
      }),
    );
  });
  const runs = await lab({ case: "listRuns", value: {} });
  expect(runs.runs.map(run => run.id)).toEqual(["research-1"]);
});

test("a service error and a reply to another request are errors", async () => {
  serve(request =>
    toBinary(
      ResponseSchema,
      create(ResponseSchema, {
        id: request.id,
        result: {
          case: "error",
          value: { code: ErrorCode.INVALID_REQUEST, message: "Market settings missing" },
        },
      }),
    ),
  );
  await expect(lab({ case: "getDatasetSummary", value: {} })).rejects.toMatchObject({
    code: "invalid_request",
  });
  serve(() => toBinary(ResponseSchema, create(ResponseSchema, { id: 0n })));
  await expect(lab({ case: "getDatasetSummary", value: {} })).rejects.toBeInstanceOf(BackendError);
});
