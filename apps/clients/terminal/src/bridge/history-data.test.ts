import { create, fromJson, toJson } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { RequestSchema, ResponseSchema, Table } from "../generated/asterion/history_pb";
import { BackendError } from "../i18n/errors";
import { historyData } from "./history-data";

const call = vi.hoisted(() => vi.fn());
vi.mock("./client", () => ({ call }));

test("History's generated mapping preserves 64-bit counts across the native boundary", async () => {
  call.mockImplementation(async (method, params) => {
    expect(method).toBe("history.call");
    expect(fromJson(RequestSchema, params.request).call.case).toBe("getDatasetSummary");
    return {
      history: toJson(
        ResponseSchema,
        create(ResponseSchema, {
          result: {
            case: "getDatasetSummary",
            value: { tables: [{ table: Table.DAILY, rows: 9007199254740993n }] },
          },
        }),
      ),
    };
  });
  const result = await historyData({ case: "getDatasetSummary", value: {} });
  expect(result.tables[0].rows).toBe(9007199254740993n);
});

test("a denied or ambiguous write is surfaced once without automatically retrying", async () => {
  call.mockRejectedValueOnce(new BackendError("permission_denied", "read only"));
  await expect(
    historyData({ case: "cancelAcquisition", value: { id: "one" } }),
  ).rejects.toMatchObject({ code: "permission_denied" });
  call.mockRejectedValueOnce(new Error("connection lost"));
  const before = call.mock.calls.length;
  await expect(historyData({ case: "startAcquisition", value: { id: "two" } })).rejects.toThrow(
    "connection lost",
  );
  expect(call.mock.calls.length).toBe(before + 1);
});
