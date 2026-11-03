import { create, fromJson, toJson } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { RequestSchema, ResponseSchema } from "../generated/asterion/market_pb";
import { BackendError } from "../i18n/errors";
import { marketData } from "./market-data";

const call = vi.hoisted(() => vi.fn());
vi.mock("./client", () => ({ call }));

test("Market's generated mapping preserves 64-bit counts across the native boundary", async () => {
  call.mockImplementation(async (method, params) => {
    expect(method).toBe("market.call");
    expect(fromJson(RequestSchema, params.request).operation.case).toBe("datasetSummary");
    return {
      market: toJson(
        ResponseSchema,
        create(ResponseSchema, {
          result: {
            case: "datasetSummary",
            value: { provider: "shfe", tables: [{ table: "daily", rows: 9007199254740993n }] },
          },
        }),
      ),
    };
  });
  const result = await marketData(
    { case: "datasetSummary", value: { provider: "shfe" } },
    "datasetSummary",
  );
  expect(result.tables[0].rows).toBe(9007199254740993n);
});

test("a denied or ambiguous write is surfaced once without automatically retrying", async () => {
  call.mockRejectedValueOnce(new BackendError("permission_denied", "read only"));
  await expect(
    marketData({ case: "acquisitionCancel", value: { id: "one" } }, "acquisitionTask"),
  ).rejects.toMatchObject({ code: "permission_denied" });
  call.mockRejectedValueOnce(new Error("connection lost"));
  const before = call.mock.calls.length;
  await expect(
    marketData({ case: "acquisitionStart", value: { id: "two" } }, "acquisitionTask"),
  ).rejects.toThrow("connection lost");
  expect(call.mock.calls.length).toBe(before + 1);
});
