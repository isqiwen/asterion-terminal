import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
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
import { RunComparison, RunResult, SweepResult } from "./ResearchResult";
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

test("reproduction details omit the commit suffix when the strategy has no git commit", async () => {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case !== "getRun") throw new Error("Unexpected operation");
    return create(RunDetailSchema, {
      summary: { id: "no-git", state: RunState.SUCCEEDED },
      record: JSON.stringify({
        strategy: { id: "ma_cross.MaCross", commit: "", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "a".repeat(64),
          symbols: ["SHFE/au/2025-12"],
          begin_day: "2025-01-02",
          end_day: "2025-12-15",
          bars: { period: "day" },
        },
      }),
    }) as never;
  });
  render(<RunResult id="no-git" state="succeeded" />);
  fireEvent.click(await screen.findByRole("button", { name: "复现信息" }));
  const strategy = screen.getByText("策略").nextElementSibling!;
  expect(strategy.textContent).toBe("ma_cross.MaCross");
});

test("holdout metrics render in one compact comparison table instead of duplicate figure strips", async () => {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case !== "getRun") throw new Error("Unexpected operation");
    return create(RunDetailSchema, {
      summary: {
        id: "with-holdout",
        state: RunState.SUCCEEDED,
        metrics: {
          total_return: 0.2,
          development_total_return: 0.15,
          development_sharpe: 1.4,
          holdout_total_return: 0.05,
          holdout_sharpe: 0.9,
        },
      },
      record: JSON.stringify({
        strategy: { id: "ma_cross.MaCross", commit: "", changes: "" },
        costs: { deposit: "1000000" },
        statistics: { holdout_day: "2026-02-01" },
        engine: { version: "test" },
        data: {
          dataset_version: "a".repeat(64),
          symbols: ["SHFE/rb"],
          begin_day: "2026-01-05",
          end_day: "2026-02-13",
          bars: { period: "day" },
        },
      }),
    }) as never;
  });
  const { container } = render(<RunResult id="with-holdout" state="succeeded" />);
  const table = await screen.findByRole("table", { name: "分段对比" });
  expect(container.querySelectorAll("dl.lab-figures")).toHaveLength(1);
  expect(within(table).getByRole("rowheader", { name: "留出日 2026-02-01 之前" })).toBeTruthy();
  expect(within(table).getByRole("rowheader", { name: "留出段" })).toBeTruthy();
  expect(within(table).getByText("15.00%")).toBeTruthy();
  expect(within(table).getByText("0.900")).toBeTruthy();
});

test("run comparison shows series swatches, differing run contexts, and date alignment notice", async () => {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case !== "getRun") throw new Error("Unexpected operation");
    const first = operation.value.id === "r1";
    return create(RunDetailSchema, {
      summary: {
        id: operation.value.id,
        strategy: first ? "a.Alpha" : "b.Beta",
        symbols: first ? ["SHFE/rb"] : ["DCE/i"],
        firstDay: first ? "2025-01-02" : "2025-06-01",
        lastDay: "2026-02-13",
      },
      record: JSON.stringify({ costs: { deposit: "1000000" } }),
    }) as never;
  });
  render(<RunComparison ids={["r1", "r2"]} />);
  const firstHeader = await screen.findByRole("columnheader", { name: "r1" });
  const secondHeader = screen.getByRole("columnheader", { name: "r2" });
  expect((firstHeader.querySelector(".lab-swatch") as HTMLElement).style.background).toBe(
    "var(--price-line)",
  );
  expect((secondHeader.querySelector(".lab-swatch") as HTMLElement).style.background).toBe(
    "var(--series-1)",
  );
  expect(screen.getByRole("row", { name: /策略 a\.Alpha b\.Beta/ })).toBeTruthy();
  expect(screen.getByRole("row", { name: /品种 SHFE\/rb DCE\/i/ })).toBeTruthy();
  expect(screen.getByText("净值曲线按首次运行（r1）的交易日对齐")).toBeTruthy();
});

test("diagnostics tab renders trade quality, symbol/sector attribution, and combined strategy components", async () => {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case !== "getRun") throw new Error("Unexpected operation");
    return create(RunDetailSchema, {
      summary: { id: "diag-run", state: RunState.SUCCEEDED },
      record: JSON.stringify({
        strategy: { id: "composite.CompositePortfolio", commit: "", changes: "" },
        costs: { deposit: "1000000" },
        engine: { version: "test" },
        data: {
          dataset_version: "a".repeat(64),
          symbols: ["SHFE/rb", "SHFE/cu"],
          begin_day: "2025-01-02",
          end_day: "2026-02-13",
          bars: { period: "day" },
          diagnostics: {
            trades: {
              count: 20,
              wins: 13,
              losses: 7,
              win_rate: 0.65,
              payoff_ratio: 1.5,
              profit_factor: 2.25,
              avg_holding_days: 4.5,
            },
            costs: {
              gross_pnl: 50000,
              fees: 2000,
              estimated_slippage: 1500,
              net_pnl: 48000,
              annual_turnover: 8.5,
            },
            exposure: {
              peak_margin_ratio: 0.28,
              peak_gross_leverage: 1.4,
              avg_net_exposure: 0.12,
              max_drawdown_days: 14,
            },
            direction: { long_pnl: 30000, short_pnl: 18000 },
            significance: {
              n_days: 120,
              trials: 5,
              skewness: 0.25,
              excess_kurtosis: 0.4,
              psr: 0.96,
              dsr: 0.84,
              nw_t: 2.35,
              nw_p: 0.021,
              bootstrap: {
                alpha: 0.05,
                block_size: 5,
                samples: 500,
                sharpe_ci: [0.45, 2.15],
                annual_return_ci: [0.04, 0.28],
                max_drawdown_ci: [-0.12, -0.03],
              },
            },
            by_symbol: [
              {
                symbol: "SHFE/rb",
                sector: "black",
                gross_pnl: 35000,
                fees: 1200,
                slippage_cost: 900,
                net_pnl: 33800,
                long_pnl: 20000,
                short_pnl: 13800,
                fills: 12,
              },
              {
                symbol: "SHFE/cu",
                sector: "nonferrous",
                gross_pnl: 15000,
                fees: 800,
                slippage_cost: 600,
                net_pnl: 14200,
                long_pnl: 10000,
                short_pnl: 4200,
                fills: 8,
              },
            ],
            by_sector: [
              {
                sector: "black",
                gross_pnl: 35000,
                fees: 1200,
                slippage_cost: 900,
                net_pnl: 33800,
                long_pnl: 20000,
                short_pnl: 13800,
                fills: 12,
              },
              {
                sector: "nonferrous",
                gross_pnl: 15000,
                fees: 800,
                slippage_cost: 600,
                net_pnl: 14200,
                long_pnl: 10000,
                short_pnl: 4200,
                fills: 8,
              },
            ],
            by_session: [
              {
                session: "night",
                trades: 4,
                wins: 3,
                win_rate: 0.75,
                gross_pnl: 12000,
                fees: 400,
                slippage_cost: 300,
                net_pnl: 11600,
                fills: 8,
              },
              {
                session: "opening_15m",
                trades: 2,
                wins: 1,
                win_rate: 0.5,
                gross_pnl: 4000,
                fees: 200,
                slippage_cost: 150,
                net_pnl: 3800,
                fills: 4,
              },
            ],
            components: [
              {
                name: "MultiFactorRank",
                share: 0.6,
                ready_rate: 0.9,
                active_bars: 18,
                total_bars: 20,
                avg_gross_weight: 0.5,
                avg_contribution_weight: 0.3,
                proxy_return: 0.04,
              },
            ],
          },
        },
      }),
    }) as never;
  });
  render(<RunResult id="diag-run" state="succeeded" />);
  fireEvent.click(await screen.findByRole("button", { name: "归因与诊断" }));
  expect(screen.getByText("65.00%")).toBeTruthy();
  expect(screen.getByText("2.250")).toBeTruthy();
  expect(screen.getByText("14 天")).toBeTruthy();
  expect(screen.getByRole("table", { name: "分品种归因" })).toBeTruthy();
  expect(screen.getByRole("table", { name: "分板块归因" })).toBeTruthy();
  const sess = screen.getByRole("table", { name: "日内分时段归因" });
  expect(within(sess).getByRole("rowheader", { name: "夜盘" })).toBeTruthy();
  expect(within(sess).getByRole("rowheader", { name: "开盘 15m" })).toBeTruthy();
  const comp = screen.getByRole("table", { name: "子策略过程与贡献" });
  expect(within(comp).getByRole("rowheader", { name: "MultiFactorRank" })).toBeTruthy();
  expect(screen.getByText("统计显著性与稳健性")).toBeTruthy();
  expect(screen.getByText("PSR 置信度")).toBeTruthy();
  expect(screen.getByText("DSR 置信度")).toBeTruthy();
  expect(screen.getByText("Newey-West t")).toBeTruthy();
  expect(screen.getByText("夏普 95% 区间")).toBeTruthy();
});

test("sweep result renders slippage break-even summary and parameter overfitting check", () => {
  const { rerender } = render(
    <SweepResult
      runs={[
        create(RunSummarySchema, {
          id: "s-0",
          state: RunState.SUCCEEDED,
          params: { __slippage__: { value: { case: "integer", value: 0n } } },
          metrics: { sharpe: 1.2, total_return: 0.2, max_drawdown: 0.05 },
        }),
        create(RunSummarySchema, {
          id: "s-1",
          state: RunState.SUCCEEDED,
          params: { __slippage__: { value: { case: "integer", value: 2n } } },
          metrics: { sharpe: 0.4, total_return: 0.05, max_drawdown: 0.07 },
        }),
        create(RunSummarySchema, {
          id: "s-2",
          state: RunState.SUCCEEDED,
          params: { __slippage__: { value: { case: "integer", value: 4n } } },
          metrics: { sharpe: -0.4, total_return: -0.05, max_drawdown: 0.12 },
        }),
      ]}
      selected=""
      open={vi.fn()}
    />,
  );
  expect(screen.getByText("盈亏平衡滑点")).toBeTruthy();
  expect(screen.getByText("3.0 跳")).toBeTruthy();

  rerender(
    <SweepResult
      runs={[
        create(RunSummarySchema, {
          id: "p-0",
          state: RunState.SUCCEEDED,
          firstDay: "2024-01-02",
          lastDay: "2026-01-02",
          params: { fast: { value: { case: "integer", value: 5n } } },
          metrics: { sharpe: 1.0, total_return: 0.1, max_drawdown: 0.05 },
        }),
        create(RunSummarySchema, {
          id: "p-1",
          state: RunState.SUCCEEDED,
          firstDay: "2024-01-02",
          lastDay: "2026-01-02",
          params: { fast: { value: { case: "integer", value: 10n } } },
          metrics: { sharpe: 1.6, total_return: 0.2, max_drawdown: 0.04 },
        }),
        create(RunSummarySchema, {
          id: "p-2",
          state: RunState.SUCCEEDED,
          firstDay: "2024-01-02",
          lastDay: "2026-01-02",
          params: { fast: { value: { case: "integer", value: 15n } } },
          metrics: { sharpe: 1.4, total_return: 0.18, max_drawdown: 0.05 },
        }),
      ]}
      selected=""
      open={vi.fn()}
      pbo={0.17}
      pboRankCorr={0.62}
    />,
  );
  expect(screen.getByText("DSR 置信度")).toBeTruthy();
  expect(screen.getByText("高原稳健度")).toBeTruthy();
  // Neighbors of fast=10 (1.6) are 1.0 and 1.4 -> median 1.2 -> plateau ratio 75.00%.
  expect(screen.getByText("75.00%")).toBeTruthy();
  expect(screen.getByText("PBO 过拟合概率")).toBeTruthy();
  expect(screen.getByText("17.00%")).toBeTruthy();
  expect(screen.getByText("CSCV 秩相关")).toBeTruthy();
  expect(screen.getByText("0.620")).toBeTruthy();
});

test("3D sweep renders selectable 2D heatmap axes and plateau-robustness sorting", () => {
  // 2x2x2 sweep where (a=1, b=1, c=1) is a fragile spike (2.5 with poor neighbors 0.1)
  // while (a=2, b=2, c=2) sits on a strong plateau (2.0 with neighbors ~1.8).
  const cases = [
    ["r0", 1, 1, 1, 2.5],
    ["r1", 1, 1, 2, 0.1],
    ["r2", 1, 2, 1, 0.1],
    ["r3", 2, 1, 1, 0.1],
    ["r4", 1, 2, 2, 1.8],
    ["r5", 2, 1, 2, 1.8],
    ["r6", 2, 2, 1, 1.8],
    ["r7", 2, 2, 2, 2.0],
  ] as const;
  render(
    <SweepResult
      runs={cases.map(([id, a, b, c, sharpe]) =>
        create(RunSummarySchema, {
          id,
          state: RunState.SUCCEEDED,
          firstDay: "2024-01-02",
          lastDay: "2026-01-02",
          params: {
            a: { value: { case: "integer", value: BigInt(a) } },
            b: { value: { case: "integer", value: BigInt(b) } },
            c: { value: { case: "integer", value: BigInt(c) } },
          },
          metrics: { sharpe, total_return: sharpe * 0.1, max_drawdown: 0.05 },
        }),
      )}
      selected=""
      open={vi.fn()}
    />,
  );
  // 3D sweep shows selectable heatmap axis pickers and renders the 2D projection heatmap.
  expect(screen.getByLabelText("行轴")).toBeTruthy();
  expect(screen.getByLabelText("列轴")).toBeTruthy();
  expect(screen.getByRole("table", { name: "参数热力图" })).toBeTruthy();

  // Switch column axis to c and sort by plateau robustness -> plateau candidate r7 rises above spike r0.
  fireEvent.change(screen.getByLabelText("列轴"), { target: { value: "c" } });
  expect(
    within(screen.getByRole("table", { name: "参数热力图" })).getByText("a \\ c"),
  ).toBeTruthy();
  fireEvent.change(screen.getByLabelText("排序依据"), { target: { value: "plateau" } });
  const ranking = screen.getByRole("region", { name: "排名" });
  const firstRow = within(ranking).getAllByRole("row")[1]!;
  expect(firstRow.textContent).toContain("2.000");
});

test("run comparison computes correlation matrix, portfolio simulation, and generates a CombinedStrategy", async () => {
  const saved: Array<{ path?: string; content?: string }> = [];
  const onGenerated = vi.fn();
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case === "saveSource") {
      saved.push({ path: operation.value.source?.path, content: operation.value.content });
      return {} as never;
    }
    if (operation.case !== "getRun") throw new Error("Unexpected operation");
    const first = operation.value.id === "r1";
    const equities = first ? [100, 102, 101, 104, 106] : [100, 99, 102, 101, 105];
    return create(RunDetailSchema, {
      summary: {
        id: operation.value.id,
        strategy: first ? "trend.Trend" : "carry.Carry",
        symbols: first ? ["SHFE/rb"] : ["SHFE/cu"],
        firstDay: "2026-01-05",
        lastDay: "2026-01-09",
      },
      record: JSON.stringify({
        strategy: { id: first ? "trend.Trend" : "carry.Carry", repository: "/research" },
        params: first ? { lookback: 20 } : { window: 10 },
        costs: { deposit: "100" },
      }),
      days: equities.map((eq, idx) => ({
        tradingDay: `2026-01-0${idx + 5}`,
        equity: { text: String(eq) },
        settledMs: BigInt((idx + 1) * 86400_000),
      })),
    }) as never;
  });
  render(<RunComparison ids={["r1", "r2"]} repository="/research" onGenerated={onGenerated} />);
  expect(await screen.findByRole("table", { name: "策略相关性矩阵" })).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "波动率平价" }));
  fireEvent.click(screen.getByRole("button", { name: "生成组合策略" }));
  fireEvent.click(screen.getByRole("button", { name: "创建" }));
  await waitFor(() => expect(saved).toHaveLength(1));
  expect(saved[0]!.content).toContain("class TrendCarryPortfolio(CombinedStrategy):");
  expect(saved[0]!.content).toContain("vol_balance = True");
  expect(onGenerated).toHaveBeenCalledWith(
    "strategies/trend_carry_portfolio.py",
    "trend_carry_portfolio.TrendCarryPortfolio",
    { symbols: "SHFE/rb, SHFE/cu", begin: "2026-01-05", end: "2026-01-09" },
  );
});
