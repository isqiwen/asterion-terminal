import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import type { TradingConnection } from "../../src/bridge/client";
import { lab } from "../../src/bridge/lab";
import { tradingCall } from "../../src/bridge/trading";
import { Offset, Period, Side } from "../../src/generated/asterion/common_pb";
import { ReconcileResultSchema } from "../../src/generated/asterion/lab_pb";
import {
  BarSchema,
  BarsSchema,
  DecisionSchema,
  InstanceSchema,
} from "../../src/generated/asterion/strategy_pb";
import {
  DaySchema,
  RecordedBarsSchema,
  StrategiesSchema,
  StrategyRunSchema,
} from "../../src/generated/asterion/trading_pb";
import { ReconcileView } from "./ReconcileView";
import { readConditions } from "./research-lab";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
vi.mock("../../src/bridge/trading", () => ({ tradingCall: vi.fn() }));
registerTerminalFeatures([feature]);

beforeEach(() => {
  localStorage.clear();
  vi.mocked(lab).mockReset();
  vi.mocked(tradingCall).mockReset();
});

const account: TradingConnection = {
  id: "acct-sim",
  instance: "SimNow-1",
  host: "127.0.0.1",
  port: 7443,
  fingerprint: "fp",
  revision: "1",
};

test("reconciles paginated live records, renders diffs, and applies slippage to backtest conditions", async () => {
  const inst = { exchange: "SHFE", symbol: "rb2605" };
  const configuration = create(InstanceSchema, {
    id: "inst-1",
    packageName: "ma_cross.MaCross",
    packageVersion: "20260101-000000",
    period: Period.DAY,
    instruments: [inst],
    warmup: { datasetVersion: "a".repeat(64), sha256: "b".repeat(64) },
  });
  const bar = create(BarSchema, {
    instrument: inst,
    tradingDay: "2026-01-05",
    endMs: 1767596400000n,
    open: { text: "3500" },
    high: { text: "3520" },
    low: { text: "3490" },
    close: { text: "3510" },
    volume: 100n,
  });
  const recordedBars = Array.from({ length: 501 }, (_, index) =>
    create(BarSchema, { ...bar, endMs: bar.endMs + BigInt(index) * 60000n }),
  );
  const trades = Array.from({ length: 201 }, (_, index) => ({
    id: `trd-${index}`,
    orderId: `ord-${index}`,
    instrument: inst,
    side: Side.BUY,
    offset: Offset.OPEN,
    lots: { text: "2" },
    price: { text: "3512" },
    tradingDay: "2026-01-05",
  }));

  vi.mocked(tradingCall).mockImplementation(async (_account, op) => {
    switch (op.case) {
      case "getStrategies":
        return create(StrategiesSchema, {
          packages: [
            {
              name: "ma_cross.MaCross",
              version: "20260101-000000",
              research: { runId: "bt-1", datasetVersion: "a".repeat(64) },
            },
          ],
          instance: { instance: configuration, completedSteps: 1n },
        }) as never;
      case "getStrategyRun":
        return create(StrategyRunSchema, {
          configuration,
          steps: [
            {
              input: create(BarsSchema, { sequence: 1n, bars: [bar] }),
              decision: create(DecisionSchema, { sequence: 1n, ready: true }),
            },
          ],
        }) as never;
      case "getRecordedBars":
        if (!op.value.limit || op.value.limit > 500)
          throw new Error("recorded bar pages contain 1 to 500 bars");
        return create(RecordedBarsSchema, {
          bars: recordedBars
            .filter(item => item.endMs > (op.value.afterEndMs ?? 0n))
            .slice(0, op.value.limit),
        }) as never;
      case "getDay":
        if (!op.value.limit || op.value.limit > 200)
          throw new Error("trade pages contain 1 to 200 records");
        return create(DaySchema, {
          tradingDays: ["2026-01-05"],
          total: trades.length,
          trades: trades.slice(op.value.offset ?? 0, (op.value.offset ?? 0) + op.value.limit),
        }) as never;
    }
    return {} as never;
  });

  const report = {
    ok: false,
    metadata: {
      instance_id: "inst-1",
      run_id: "bt-1",
      package: "ma_cross.MaCross/20260101-000000",
      dataset_version: "a".repeat(64),
      period: "day",
      symbols: ["SHFE/rb/2026-05"],
      slippage_ticks: 0,
      window_begin_day: "2026-01-05",
      window_end_day: "2026-01-05",
      fill_window_begin: "2026-01-05",
    },
    summary: {
      bars: {
        matched: 1,
        missing_in_live: 0,
        extra_in_live: 0,
        mismatched: 0,
        max_close_diff: "0",
        max_volume_diff: "0",
        ok: true,
      },
      recorded_bars: {
        matched: 1,
        missing_in_live: 0,
        extra_in_live: 0,
        mismatched: 0,
        max_close_diff: "0",
        max_volume_diff: "0",
        ok: true,
      },
      decisions: {
        matched: 1,
        missing_in_live: 0,
        extra_in_live: 0,
        mismatched: 0,
        ok: true,
      },
      fills: {
        live_fill_count: 1,
        backtest_fill_count: 1,
        matched: 0,
        live_only: 0,
        backtest_only: 0,
        mismatched: 1,
        max_price_diff: "2",
        mean_adverse_price: "2",
        slippage_samples: 1,
        mean_slippage_ticks: 1.5,
        median_slippage_ticks: 1.5,
        p90_slippage_ticks: 1.8,
        max_slippage_ticks: 2.0,
        ok: false,
      },
    },
    bar_diffs: [],
    recorded_bar_diffs: [],
    decision_diffs: [],
    fill_diffs: [
      {
        kind: "fill_mismatch",
        trading_day: "2026-01-05",
        contract: "SHFE/rb/2026-05",
        side: "buy",
        offset: "open",
        live_qty: "2",
        backtest_qty: "2",
        live_vwap: "3512",
        backtest_vwap: "3510",
        price_delta: "2",
        adverse_price: "2",
        adverse_ticks: 2.0,
      },
    ],
  };

  vi.mocked(lab).mockResolvedValue(
    create(ReconcileResultSchema, {
      ok: false,
      report: JSON.stringify(report),
    }) as never,
  );

  const openRun = vi.fn();
  render(
    <ReconcileView
      accounts={[account]}
      repository="/tmp/repo"
      initialAccount="acct-sim"
      autoStart
      back={vi.fn()}
      openRun={openRun}
    />,
  );

  await screen.findByText("发现差异");
  expect(vi.mocked(lab)).toHaveBeenCalledWith(
    expect.objectContaining({
      case: "reconcileLive",
      value: expect.objectContaining({
        runId: "bt-1",
        repository: "/tmp/repo",
        recordedBars,
      }),
    }),
  );
  const reconciliation = vi.mocked(lab).mock.calls[0]?.[0];
  expect(reconciliation?.case).toBe("reconcileLive");
  if (reconciliation?.case === "reconcileLive")
    expect(reconciliation.value.liveFills).toHaveLength(201);
  expect(screen.getByText("+1.50 / +1.80 跳")).toBeTruthy();
  expect(screen.getByText("成交与滑点差异（1）")).toBeTruthy();

  fireEvent.click(screen.getByRole("button", { name: "应用实盘滑点到回测条件（2 跳）" }));
  await waitFor(() => expect(screen.getByText("已将回测条件滑点更新为 2 跳")).toBeTruthy());
  expect(readConditions().current?.slippage).toBe("2");

  fireEvent.click(screen.getByRole("button", { name: "查看对照回测" }));
  expect(openRun).toHaveBeenCalledWith("bt-1");

  fireEvent.change(screen.getByRole("textbox", { name: "策略实例" }), {
    target: { value: "absent" },
  });
  expect(screen.queryByRole("button", { name: "复制摘要" })).toBeNull();
  vi.mocked(tradingCall).mockRejectedValueOnce(new Error("no recorded strategy instance: absent"));
  fireEvent.click(screen.getByRole("button", { name: "开始对账" }));
  expect((await screen.findByRole("alert")).textContent).toContain(
    "no recorded strategy instance: absent",
  );
  expect(screen.queryByText("发现差异")).toBeNull();
  expect(screen.queryByRole("button", { name: "查看对照回测" })).toBeNull();
});
