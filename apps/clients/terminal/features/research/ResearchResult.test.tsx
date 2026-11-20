import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import { Offset, Period } from "../../src/generated/asterion/common_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import {
  ChartBarsSchema,
  RunDetailSchema,
  RunState,
  RunSummarySchema,
} from "../../src/generated/asterion/lab_pb";
import { RunComparison, RunResult } from "./ResearchResult";
import { feature } from "./feature";
import { BackendError } from "../../src/i18n/errors";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));
registerTerminalFeatures([feature]);

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
    if (previous === "error")
      await waitFor(() => expect(screen.queryByText("正在读取结果…")).toBeNull());
    else await screen.findByRole("columnheader", { name: "first" });

    view.rerender(<RunComparison ids={["third", "fourth"]} />);
    expect(screen.getByText("正在读取结果…")).not.toBeNull();
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
  fireEvent.click(await screen.findByRole("button", { name: "K 线与买卖点" }));
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      case: "getChartBars",
      value: { contract: "SHFE/rb/2026-05", ...expected },
    }),
  );
  expect(await screen.findByText("返回的 K 线与运行使用的数据版本不一致。")).toBeDefined();
});

test("a minute run distinguishes incomplete bars, absent periods and reported zero volume", async () => {
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
          minute_coverage: [
            {
              contract: "SHFE/rb/2026-05",
              missing: { "2026-01-05": 2, "2026-01-06": 3 },
              zero_volume_minutes: 4,
              unchecked_days: ["2026-01-05"],
            },
          ],
        },
      }),
    }) as never;
  });
  render(<RunResult id="partial" state="succeeded" />);
  expect(await screen.findByText("1 根 K 线含不完整的分钟，未参与回测：2026-01-06")).toBeDefined();
  fireEvent.click(screen.getByText("SHFE/rb/2026-05 有 5 个 K 线周期整段缺记录"));
  expect(screen.getByText("2026-01-06 缺 3 个周期").closest("details")?.open).toBe(true);
  expect(screen.getByText("SHFE/rb/2026-05 有 4 条零成交分钟记录")).toBeDefined();
  expect(screen.getByText("SHFE/rb/2026-05 有待核实的交易时段：2026-01-05")).toBeDefined();
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
          contract: "DCE/m/2026-05",
          offset: Offset.CLOSE,
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
          symbols: ["DCE/m/2026-05"],
          begin_day: "2026-01-05",
          end_day: "2026-01-06",
          bars: { period: "1m" },
        },
      }),
    }) as never;
  });
  render(<RunResult id="night" state="succeeded" />);
  fireEvent.click(await screen.findByRole("button", { name: "成交（1 笔）" }));
  const row = (await screen.findByText("2026-01-05", { selector: "td" })).closest("tr")!;
  expect(row.textContent).toContain("01-02 21:05");
  expect(row.textContent).toContain("平仓");
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
    await screen.findByText("目标被风控或资金拒绝 14 次，实盘会告警并放弃当批，实例继续运行"),
  ).not.toBeNull();
  expect(screen.getByRole("cell", { name: "超过总持仓上限" })).not.toBeNull();
  expect(screen.getByRole("cell", { name: "14" })).not.toBeNull();
});

test("a failed historical chart request leaves the stored run metrics and fills usable", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case === "getChartBars")
      throw new BackendError("unavailable", "Warehouse disconnected");
    if (operation.case !== "getRun") throw new Error("Unexpected operation");
    return create(RunDetailSchema, {
      summary: { id: "stored-result", state: RunState.SUCCEEDED, metrics: { total_return: 0.125 } },
      fills: [
        { contract: "SHFE/rb/2027-01", timeMs: 1n, lots: { text: "1" }, price: { text: "3000" } },
      ],
      record: JSON.stringify({
        strategy: { id: "example.Strategy", commit: "commit", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "v1",
          symbols: ["SHFE/rb/2027-01"],
          begin_day: "2026-11-01",
          end_day: "2026-11-14",
          bars: { period: "day" },
        },
      }),
    }) as never;
  });
  render(<RunResult id="stored-result" state="succeeded" />);
  expect(await screen.findByText("12.50%")).not.toBeNull();
  expect(vi.mocked(lab).mock.calls.some(([op]) => op.case === "getChartBars")).toBe(false);
  fireEvent.click(screen.getByRole("button", { name: "K 线与买卖点" }));
  await screen.findByRole("button", { name: "重试" });
  expect(screen.queryByText("12.50%")).not.toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "成交（1 笔）" }));
  expect(screen.getByRole("cell", { name: "SHFE/rb/2027-01" })).toBeTruthy();
});

test("large fill tables virtualize DOM rows while keeping sticky headers and visible fills", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case !== "getRun") throw new Error("Unexpected operation");
    return create(RunDetailSchema, {
      summary: { id: "many-fills", state: RunState.SUCCEEDED },
      fills: Array.from({ length: 500 }, (_, index) => ({
        id: `fill-${index}`,
        contract: `SHFE/rb/2026-05`,
        timeMs: BigInt(index + 1) * 60_000n,
        tradingDay: "2026-01-05",
        lots: { text: String(index + 1) },
        price: { text: "3000" },
      })),
      record: JSON.stringify({
        strategy: { id: "example.Strategy", commit: "commit", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "v1",
          symbols: ["SHFE/rb/2026-05"],
          begin_day: "2026-01-05",
          end_day: "2026-01-06",
          bars: { period: "1m" },
        },
      }),
    }) as never;
  });
  const view = render(<RunResult id="many-fills" state="succeeded" />);
  fireEvent.click(await screen.findByRole("button", { name: "成交（500 笔）" }));
  const region = screen.getByRole("region", { name: "成交（500 笔）" });
  expect(region.getAttribute("tabindex")).toBe("0");
  const renderedRows = view.container.querySelectorAll("tbody tr:not([data-spacer])");
  expect(renderedRows.length).toBeGreaterThan(0);
  expect(renderedRows.length).toBeLessThan(60);
  expect(screen.getByRole("cell", { name: "1" })).not.toBeNull();
  expect(screen.queryByRole("cell", { name: "500" })).toBeNull();
});
