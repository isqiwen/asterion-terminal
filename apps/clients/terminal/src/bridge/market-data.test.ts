import { create, fromJson, toJson } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { RequestSchema, ResponseSchema } from "../generated/asterion/market_pb";
import { marketData } from "./market-data";

const request = vi.hoisted(() => vi.fn());
vi.mock("./client", () => ({ request }));

test("Market's generated mapping preserves 64-bit counts across the native boundary", async () => {
  request.mockImplementation(async (method, params) => {
    expect(method).toBe("market.data");
    const message = fromJson(RequestSchema, params.request);
    expect(message.operation.case).toBe("datasetSummary");
    return {
      market_data: toJson(
        ResponseSchema,
        create(ResponseSchema, {
          version: 1,
          serviceId: message.serviceId,
          correlationId: message.correlationId,
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
  request.mockImplementationOnce(async (_method, params) => {
    const message = fromJson(RequestSchema, params.request);
    return {
      market_data: toJson(
        ResponseSchema,
        create(ResponseSchema, {
          version: 1,
          serviceId: message.serviceId,
          correlationId: message.correlationId,
          result: { case: "error", value: { code: "permission_denied", message: "read only" } },
        }),
      ),
    };
  });
  await expect(
    marketData({ case: "acquisitionCancel", value: { id: "one" } }, "acquisitionTask"),
  ).rejects.toMatchObject({ code: "permission_denied" });
  request.mockRejectedValueOnce(new Error("connection lost"));
  const before = request.mock.calls.length;
  await expect(
    marketData({ case: "acquisitionStart", value: { id: "two" } }, "acquisitionTask"),
  ).rejects.toThrow("connection lost");
  expect(request.mock.calls.length).toBe(before + 1);
});

test("mismatched response identities and operation types fail explicitly", async () => {
  request.mockResolvedValueOnce({
    market_data: {
      version: 1,
      serviceId: "market-data",
      correlationId: "other",
      acquisitionSources: {},
    },
  });
  await expect(
    marketData({ case: "acquisitionSources", value: {} }, "acquisitionSources"),
  ).rejects.toThrow("identity mismatch");
  request.mockImplementationOnce(async (_method, params) => {
    const message = fromJson(RequestSchema, params.request);
    return {
      market_data: {
        version: 1,
        serviceId: message.serviceId,
        correlationId: message.correlationId,
        datasets: {},
      },
    };
  });
  await expect(
    marketData({ case: "acquisitionSources", value: {} }, "acquisitionSources"),
  ).rejects.toThrow("another operation");
});
