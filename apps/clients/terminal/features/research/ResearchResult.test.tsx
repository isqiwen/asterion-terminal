import { act, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../contract";
import {
  ChartBarsSchema,
  RunDetailSchema,
  RunSummarySchema,
} from "../../src/generated/asterion/lab_pb";
import { RunComparison, RunResult } from "./ResearchResult";
import "./feature";

vi.mock("../contract", async original => ({
  ...(await original<typeof import("../contract")>()),
  lab: vi.fn(),
  translate: (_namespace: string, key: string) => key,
}));
vi.mock("@asterion/client-ui/PriceChart", () => ({ PriceChart: () => null }));

test("status refreshes preserve a pending comparison of the same two runs", async () => {
  let release!: () => void;
  const pending = new Promise<void>(resolve => {
    release = resolve;
  });
  const request = vi.mocked(lab);
  request.mockImplementation(async operation => {
    await pending;
    if (operation?.case !== "run") throw new Error("expected run read");
    return create(RunDetailSchema, {
      summary: create(RunSummarySchema, { id: operation.value.id }),
      record: JSON.stringify({ costs: { deposit: "1000000" } }),
    }) as never;
  });
  const view = render(<RunComparison ids={["first", "second"]} />);
  // A refreshed parent reconstructs the selection while both reads are pending.
  view.rerender(<RunComparison ids={["first", "second"]} />);
  view.rerender(<RunComparison ids={["first", "second"]} />);
  expect(request).toHaveBeenCalledTimes(2);
  await act(async () => {
    release();
    await pending;
  });
  expect(screen.getByRole("columnheader", { name: "first" })).not.toBeNull();
  expect(screen.getByRole("columnheader", { name: "second" })).not.toBeNull();
});

test.each([
  { period: "day", datasetVersion: "a".repeat(64) },
  { period: "5", datasetVersion: "a".repeat(64) },
])("the result chart reads the run's research version for $period bars", async expected => {
  const request = vi.mocked(lab);
  request.mockReset();
  request.mockImplementation(async operation => {
    if (operation?.case === "chartBars")
      return create(ChartBarsSchema, { datasetVersion: "another-version" }) as never;
    if (operation?.case !== "run") throw new Error("expected run read");
    return create(RunDetailSchema, {
      summary: { id: "pinned", state: "succeeded" },
      fills: [{ contract: "SHFE/rb/2026-05", timeNs: 1n, quantity: "1", price: "3000" }],
      record: JSON.stringify({
        strategy: { id: "ma_cross.MaCross", commit: "commit", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "a".repeat(64),
          symbols: ["SHFE/rb/2026-05"],
          begin_day: "2026-01-05",
          end_day: "2026-02-13",
          bars: { period: expected.period === "day" ? "day" : "5m" },
        },
      }),
    }) as never;
  });
  render(<RunResult id="pinned" state="succeeded" />);
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith(
      {
        case: "chartBars",
        value: { contract: "SHFE/rb/2026-05", ...expected },
      },
      "chartBars",
    ),
  );
  expect(await screen.findByText("返回的 K 线与运行使用的数据版本不一致。")).toBeDefined();
});

test("a minute run says which bars it left out for an incomplete minute", async () => {
  const request = vi.mocked(lab);
  request.mockReset();
  request.mockImplementation(async operation => {
    if (operation?.case === "chartBars")
      return create(ChartBarsSchema, { datasetVersion: "a".repeat(64) }) as never;
    if (operation?.case !== "run") throw new Error("expected run read");
    return create(RunDetailSchema, {
      summary: { id: "partial", state: "succeeded" },
      record: JSON.stringify({
        strategy: { id: "ma_cross.MaCross", commit: "commit", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "a".repeat(64),
          symbols: ["SHFE/rb/2026-05"],
          begin_day: "2026-01-05",
          end_day: "2026-01-06",
          bars: { period: "1m" },
          excluded: { bars: 1, days: ["2026-01-06"] },
        },
      }),
    }) as never;
  });
  render(<RunResult id="partial" state="succeeded" />);
  expect(await screen.findByText("{bars} 根 K 线含不完整的分钟，未参与回测：{days}")).toBeDefined();
});
