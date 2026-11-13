import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import { Period } from "../../src/generated/asterion/common_pb";
import {
  ChartBarsSchema,
  RunDetailSchema,
  RunState,
  RunSummarySchema,
} from "../../src/generated/asterion/lab_pb";
import { RunComparison, RunResult } from "./ResearchResult";
import "./feature";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
}));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string) => key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

test("status refreshes preserve a pending comparison of the same two runs", async () => {
  let release!: () => void;
  const pending = new Promise<void>(resolve => {
    release = resolve;
  });
  const request = vi.mocked(lab);
  request.mockImplementation(async operation => {
    await pending;
    if (operation?.case !== "getRun") throw new Error("expected run read");
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

test.each(["result", "error"])(
  "changing the comparison selection clears the previous %s",
  async previous => {
    let release!: () => void;
    const pending = new Promise<void>(resolve => {
      release = resolve;
    });
    const request = vi.mocked(lab);
    request.mockReset();
    request.mockImplementation(async operation => {
      if (operation?.case !== "getRun") throw new Error("expected run read");
      const id = operation.value.id;
      if (id === "third" || id === "fourth") await pending;
      else if (previous === "error") throw new Error("previous comparison failed");
      return create(RunDetailSchema, {
        summary: { id },
        record: JSON.stringify({ costs: { deposit: "1000000" } }),
      }) as never;
    });
    const view = render(<RunComparison ids={["first", "second"]} />);
    if (previous === "error") await screen.findByText("previous comparison failed");
    else await screen.findByRole("columnheader", { name: "first" });

    view.rerender(<RunComparison ids={["third", "fourth"]} />);
    expect(screen.getByText("正在读取结果…")).not.toBeNull();
    expect(screen.queryByText("previous comparison failed")).toBeNull();
    expect(screen.queryByRole("columnheader", { name: "first" })).toBeNull();
    expect(screen.queryByRole("columnheader", { name: "second" })).toBeNull();
    await act(async () => {
      release();
      await pending;
    });
    expect(screen.getByRole("columnheader", { name: "third" })).not.toBeNull();
    expect(screen.getByRole("columnheader", { name: "fourth" })).not.toBeNull();
  },
);

test.each([
  { period: Period.DAY, datasetVersion: "a".repeat(64) },
  { period: Period.MINUTES_5, datasetVersion: "a".repeat(64) },
])("the result chart reads the run's research version for $period bars", async expected => {
  const request = vi.mocked(lab);
  request.mockReset();
  request.mockImplementation(async operation => {
    if (operation?.case === "getChartBars")
      return create(ChartBarsSchema, { datasetVersion: "another-version" }) as never;
    if (operation?.case !== "getRun") throw new Error("expected run read");
    return create(RunDetailSchema, {
      summary: { id: "pinned", state: RunState.SUCCEEDED },
      fills: [
        { contract: "SHFE/rb/2026-05", timeMs: 1n, lots: { text: "1" }, price: { text: "3000" } },
      ],
      record: JSON.stringify({
        strategy: { id: "ma_cross.MaCross", commit: "commit", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "a".repeat(64),
          symbols: ["SHFE/rb/2026-05"],
          begin_day: "2026-01-05",
          end_day: "2026-02-13",
          bars: { period: expected.period === Period.DAY ? "day" : "5m" },
        },
      }),
    }) as never;
  });
  render(<RunResult id="pinned" state="succeeded" />);
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      case: "getChartBars",
      value: { contract: "SHFE/rb/2026-05", ...expected },
    }),
  );
  expect(await screen.findByText("返回的 K 线与运行使用的数据版本不一致。")).toBeDefined();
});

test("a minute run says which bars it left out for an incomplete minute", async () => {
  const request = vi.mocked(lab);
  request.mockReset();
  request.mockImplementation(async operation => {
    if (operation?.case === "getChartBars")
      return create(ChartBarsSchema, { datasetVersion: "a".repeat(64) }) as never;
    if (operation?.case !== "getRun") throw new Error("expected run read");
    return create(RunDetailSchema, {
      summary: { id: "partial", state: RunState.SUCCEEDED },
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

test("a Friday night fill shows Monday's trading day and Friday's time", async () => {
  const request = vi.mocked(lab);
  request.mockReset();
  request.mockImplementation(async operation => {
    if (operation?.case === "getChartBars")
      return create(ChartBarsSchema, { datasetVersion: "a".repeat(64) }) as never;
    if (operation?.case !== "getRun") throw new Error("expected run read");
    return create(RunDetailSchema, {
      summary: { id: "night", state: RunState.SUCCEEDED },
      fills: [
        {
          id: "fill",
          contract: "SHFE/rb/2026-05",
          // Friday 2026-01-02 21:05 Beijing time.
          timeMs: BigInt(Date.UTC(2026, 0, 2, 13, 5)),
          tradingDay: "2026-01-05",
          lots: { text: "1" },
          price: { text: "3000" },
        },
      ],
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
        },
      }),
    }) as never;
  });
  render(<RunResult id="night" state="succeeded" />);
  fireEvent.click(await screen.findByRole("button", { name: "成交（{count} 笔）" }));
  const row = (await screen.findByText("2026-01-05", { selector: "td" })).closest("tr")!;
  expect(row.textContent).toContain("01-02 21:05");
});

test("a run whose targets were refused shows how often and why", async () => {
  const request = vi.mocked(lab);
  request.mockReset();
  request.mockImplementation(async operation => {
    if (operation?.case === "getChartBars") return create(ChartBarsSchema, {}) as never;
    if (operation?.case !== "getRun") throw new Error("expected run read");
    return create(RunDetailSchema, {
      summary: { id: "refused", state: RunState.SUCCEEDED },
      record: JSON.stringify({
        strategy: { id: "ma_cross.MaCross", commit: "commit", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "a".repeat(64),
          symbols: ["SHFE/rb/2026-05"],
          begin_day: "2026-01-05",
          end_day: "2026-02-13",
          bars: { period: "day" },
          rejections: [
            {
              contract: "SHFE/rb/2026-05",
              lots: "5",
              reason: "gross_quantity",
              count: 14,
              first_ms: 1767942000000,
              last_ms: 1769583600000,
            },
          ],
        },
      }),
    }) as never;
  });
  render(<RunResult id="refused" state="succeeded" />);
  expect(
    await screen.findByText("目标被风控或资金拒绝 {count} 次，实盘遇到会结束实例"),
  ).not.toBeNull();
  expect(screen.getByRole("cell", { name: "超过总持仓上限" })).not.toBeNull();
  expect(screen.getByRole("cell", { name: "14" })).not.toBeNull();
});
