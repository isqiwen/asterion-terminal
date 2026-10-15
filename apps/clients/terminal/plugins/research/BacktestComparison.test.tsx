import { SlippageSensitivity, matchesSlippageBaseline } from "./SlippageSensitivity";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { captureComparison } from "./ComparisonExport";
import { fireEvent, render, screen, within } from "@testing-library/react";
import { waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import {
  BacktestComparison,
  backtestComparisonReasons,
  type BacktestEvidence,
} from "./BacktestComparison";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
function evidence(id: string): BacktestEvidence {
  const risk = { max_order_quantity: "1", max_gross_quantity: "2", max_working_orders: 2 };
  const figures = {
    trading_days: 5,
    total_return: 0,
    max_drawdown: 0,
    winning_days: 0,
    annual_return: null,
    annual_volatility: null,
    sharpe: null,
    calmar: null,
  };
  return {
    id,
    kind: "backtest",
    input_sha256: "f".repeat(64),
    task: {
      id,
      kind: "backtest",
      state: "succeeded",
      risk_artifact: "a".repeat(64),
      attempt: 1,
      completed: 5,
      total: 5,
      error: "",
      result_digest: "d".repeat(64),
      trading_day: "2026-09-22 / 2026-09-28",
      instrument: "SHFE/rb",
      source_name: "fixed",
      submission_sequence: 1,
      submitted_at_ms: 1,
      updated_at_ms: 1,
    },
    experiment: {
      dataset_revision: "b".repeat(64),
      strategies: [{ quantity: "1", sides: "both", rule: { kind: "momentum", lookback: 2 } }],
      holdout_day: "",
      walk_forward: null,
      paper: {
        type: "historical_paper",
        deposit: "10000",
        risk,
        contracts: [
          {
            slippage_ticks: 0,
            cost_schedule: [
              {
                effective_from: "2026-09-21",
                source: "fixture",
                values: {
                  margin_per_lot: "100",
                  open_fee: "0",
                  close_today_fee: "0",
                  close_yesterday_fee: "0",
                  margin_rate: "0",
                  open_fee_rate: "0",
                  close_today_fee_rate: "0",
                  close_yesterday_fee_rate: "0",
                },
              },
            ],
            dataset: {
              revision: "b".repeat(64),
              source: "daily",
              source_dataset_ids: [],
              daily: {
                snapshot: "c".repeat(64),
                daily_source: "fixture.daily",
                calendar_source: "fixture.calendar",
                contracts_source: "fixture.contracts",
              },
              daily_acquired_at_ns: "1",
              interval_minutes: 0,
              contract: {
                venue: "SHFE",
                symbol: "rb2610",
                product: "rb",
                delivery_month: "2026-10",
                currency: "CNY",
                price_increment: "1",
                quantity_increment: "1",
                multiplier: "10",
              },
              price_increment_changes: [],
              history_evidence: [],
            },
          },
        ],
      },
      data: [
        {
          count: 5,
          first_timestamp_ns: "1",
          last_timestamp_ns: "5",
          first_day: "2026-09-22",
          last_day: "2026-09-28",
          interval_minutes: 0,
          source: "daily",
          source_dataset_ids: [],
        },
      ],
      series: [[{ trading_day: "2026-09-22", contract: 0, factor: "1" }]],
    },
    result: {
      dataset_revision: "b".repeat(64),
      candidates: [],
      selected: 0,
      folds: [],
      max_drawdown: "0",
      equity: [],
      settlements: [
        {
          trading_day: "2026-09-22",
          timestamp_ns: "1",
          balance: "10000",
          equity: "10000",
          realized: "0",
          fees: "0",
          contracts: [],
        },
      ],
      account: {
        mode: "backtest",
        persistent: false,
        storage_state: "ready",
        balance: "10000",
        equity: "10000",
        available: "10000",
        margin: "0",
        frozen: "0",
        fees: "0",
        realized: "0",
        unrealized: "0",
        cursor: 5,
        total: 5,
        timestamp_ns: "5",
        contracts: [],
        risk,
        positions: [],
        orders: [],
        fills: [],
      },
    },
    performance: {
      ...figures,
      by_month: [],
      out_of_sample: null,
      folds: [],
      development: null,
      holdout: null,
    },
    portfolio: {
      scope: "final_saved_account",
      settlement_path: { scope: "recorded_settlement_points", points: [] },
      contracts: [],
      net_notional: "0",
      gross_notional: "0",
      gross_quantity: "0",
      model_margin: "0",
      recorded_margin: "0",
      margin_difference: "0",
      balance: "0",
      equity: "0",
      available: "0",
      frozen: "0",
      risk: {
        max_order_quantity: "1",
        max_gross_quantity: "1",
        max_working_orders: 4,
        final_lot_headroom: "1",
      },
    },
    reconciliation: { state: "missing_fill_days", days: [], final_checks: [] },
    trades: {
      by_contract: [],
      closed: [],
      gross: "0",
      fees: "0",
      net: "0",
      open_fees: "0",
      count: 0,
      winning: 0,
      losing: 0,
      average_win: null,
      average_loss: null,
      payoff: null,
    },
  };
}
test("read-only comparison preserves zero, undefined and inapplicable metrics and full fixed evidence", async () => {
  const a = evidence("original"),
    b = evidence("variant");
  b.experiment.strategies[0] = {
    quantity: "1",
    sides: "both",
    rule: { kind: "momentum", lookback: 3 },
  };
  const before = JSON.stringify([a, b]);
  const snapshot = readySnapshot();
  snapshot.task_service!.tasks = [a.task, b.task];
  snapshot.task_result = a;
  const query = vi.fn(async (_method: string, params?: Record<string, unknown>) => ({
    ...snapshot,
    task_result: params?.id === a.id ? a : b,
  }));
  render(<BacktestComparison snapshot={snapshot} busy={false} query={query} />);
  screen.getAllByRole("checkbox").forEach(box => fireEvent.click(box));
  fireEvent.click(screen.getByRole("button", { name: "读取并比较" }));
  const table = await screen.findByRole("table", { name: "回测实验对照" });
  const row = (label: string) =>
    within(table).getByRole("rowheader", { name: label }).closest("tr")!;
  expect(row("比较口径").textContent).toContain("同口径对照");
  expect(row("策略候选与参数").textContent).toContain("不同");
  expect(row("全程绩效").textContent).toContain("0.0000%");
  expect(row("全程绩效").textContent).toContain("未定义");
  expect(row("留出段绩效").textContent).toContain("不适用");
  expect(row("固定快照").textContent).toContain("c".repeat(64));
  expect(row("风险实现摘要").textContent).toContain("a".repeat(64));
  const costs = within(row("逐日费用与保证金")).getAllByText("已固定 1 个费率版本");
  fireEvent.click(costs[0]);
  expect(row("逐日费用与保证金").textContent).toContain("2026-09-21");
  expect(query.mock.calls.map(call => call[0])).toEqual(["task.result.read", "task.result.read"]);
  expect(JSON.stringify([a, b])).toBe(before);
  expect(snapshot.task_result).toBe(a);
  const matrix = captureComparison(
    table as HTMLTableElement,
    [a, b],
    [backtestComparisonReasons(a, a), backtestComparisonReasons(a, b)],
  );
  const overall = matrix.rows.find(row => row.label === "全程绩效")!.cells[0];
  expect(overall.state).toBe("metric_group");
  expect(matrix.rows.find(row => row.label === "累计手续费")!.cells[0]).toMatchObject({
    text: "0",
    state: "defined",
  });
  expect(overall.metrics.find(metric => metric.label === "总收益率")).toMatchObject({
    state: "defined",
    text: "0.0000%",
  });
  expect(overall.metrics.find(metric => metric.label === "夏普比率")).toMatchObject({
    state: "undefined",
  });
  expect(matrix.rows.find(row => row.label === "留出段绩效")!.cells[0].state).toBe(
    "not_applicable",
  );
});
test("different cost, risk, slippage, rolls, validation, data must be independent", async () => {
  const a = evidence("base"),
    b = evidence("different");
  b.experiment.paper.contracts[0].cost_schedule[0].values.open_fee = "2";
  b.experiment.paper.risk.max_order_quantity = "2";
  b.task.risk_artifact = "e".repeat(64);
  b.experiment.paper.contracts[0].slippage_ticks = 1;
  b.experiment.series![0][0].factor = "2";
  b.experiment.holdout_day = "2026-09-24";
  b.experiment.dataset_revision = "e".repeat(64);
  b.experiment.paper.contracts[0].dataset.daily.snapshot = "e".repeat(64);
  const reasons = backtestComparisonReasons(a, b);
  expect(reasons).toEqual(
    expect.arrayContaining([
      "费用或保证金日程不同",
      "风险限额或实现不同",
      "滑点不同",
      "换月日程或调整因子不同",
      "训练或验证边界不同",
      "固定数据内容不同",
      "固定快照不同",
    ]),
  );
  const snapshot = readySnapshot();
  snapshot.task_service!.tasks = [a.task, b.task];
  render(
    <BacktestComparison
      snapshot={snapshot}
      busy={false}
      query={vi.fn(async (_method: string, p?: Record<string, unknown>) => ({
        ...snapshot,
        task_result: p?.id === a.id ? a : b,
      }))}
    />,
  );
  screen.getAllByRole("checkbox").forEach(box => fireEvent.click(box));
  fireEvent.click(screen.getByRole("button", { name: "读取并比较" }));
  const table = await screen.findByRole("table", { name: "回测实验对照" });
  expect(table.textContent).toContain("仅独立列示");
  for (const reason of reasons) expect(table.textContent).toContain(reason);
});
test("rolling boundaries and missing risk identity are never silently comparable", () => {
  const a = evidence("a"),
    b = evidence("b");
  b.experiment.walk_forward = { training_days: 2, validation_days: 1 };
  b.result.folds = [{ first_day: "2026-09-24", candidates: [], selected: null }];
  b.task.risk_artifact = "";
  expect(backtestComparisonReasons(a, b)).toEqual(
    expect.arrayContaining(["训练或验证边界不同", "风险限额或实现不同", "风险实现摘要缺失"]),
  );
});

test("holdout and rolling metrics keep their own segments and actual validation ranges", async () => {
  const a = evidence("holdout"),
    b = evidence("rolling");
  for (const v of [a, b]) {
    v.experiment.strategies.push({
      quantity: "1",
      sides: "both",
      rule: { kind: "momentum", lookback: 3 },
    });
    const day = v.result.settlements[0];
    v.result.settlements = [
      day,
      { ...day, trading_day: "2026-09-23" },
      { ...day, trading_day: "2026-09-24" },
      { ...day, trading_day: "2026-09-25" },
    ];
  }
  a.experiment.holdout_day = "2026-09-24";
  a.performance.development = { ...a.performance, total_return: 0 };
  a.performance.holdout = { ...a.performance, total_return: 0.1 };
  b.experiment.walk_forward = { training_days: 2, validation_days: 1 };
  b.result.folds = [
    { first_day: "2026-09-24", candidates: [], selected: 0 },
    { first_day: "2026-09-25", candidates: [], selected: 1 },
  ];
  b.performance.out_of_sample = { ...b.performance, total_return: 0.2 };
  const snapshot = readySnapshot();
  snapshot.task_service!.tasks = [a.task, b.task];
  render(
    <BacktestComparison
      snapshot={snapshot}
      busy={false}
      query={vi.fn(async (_method: string, p?: Record<string, unknown>) => ({
        ...snapshot,
        task_result: p?.id === a.id ? a : b,
      }))}
    />,
  );
  screen.getAllByRole("checkbox").forEach(box => fireEvent.click(box));
  fireEvent.click(screen.getByRole("button", { name: "读取并比较" }));
  const table = await screen.findByRole("table", { name: "回测实验对照" });
  const row = (name: string) => within(table).getByRole("rowheader", { name }).closest("tr")!;
  expect(row("留出段绩效").textContent).toContain("10.0000%");
  expect(row("滚动样本外绩效").textContent).toContain("20.0000%");
  expect(row("比较口径").textContent).toContain("训练或验证边界不同");
  expect(row("实际验证区间").textContent).toContain("2026-09-24 – 2026-09-24");
  expect(row("实际验证区间").textContent).toContain("2026-09-25 – 2026-09-25");
});

test("slippage sensitivity requires fixed derivation evidence and leaves general incompatibility intact", () => {
  const base = evidence("baseline");
  base.experiment.series = [];
  const variant = structuredClone(base);
  variant.id = variant.task.id = "variant";
  variant.experiment.paper.contracts[0].slippage_ticks = 2;
  variant.task.metadata = {
    revision: 1,
    name: "",
    note: "",
    source_id: base.id,
    source_instance: "task",
    source_result_digest: base.task.result_digest,
    data_choice: "fixed",
    snapshot: "",
  };
  expect(matchesSlippageBaseline(base, variant, 2)).toBe(true);
  expect(backtestComparisonReasons(base, variant)).toContain("滑点不同");
  variant.experiment.paper.deposit = "20000";
  expect(matchesSlippageBaseline(base, variant, 2)).toBe(false);
});
test("explicit slippage variants review and cancel without tasks, then create independent fixed-data jobs", async () => {
  const base = evidence("baseline");
  base.experiment.series = [];
  const snapshot = readySnapshot();
  const run = vi.fn(async (_method: string, _params?: Record<string, unknown>) => true);
  const query = vi.fn(async (_method: string, params?: Record<string, unknown>) => {
    const value = structuredClone(base);
    if (params?.id !== base.id) {
      value.id = value.task.id = String(params?.id);
      value.experiment.paper.contracts[0].slippage_ticks = Number(value.id.split("-").at(-1));
      value.task.metadata = {
        revision: 1,
        name: "",
        note: "",
        source_id: base.id,
        source_instance: "task",
        source_result_digest: base.task.result_digest,
        data_choice: "fixed",
        snapshot: "",
      };
    }
    return { ...snapshot, task_result: value };
  });
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="slippage">
      <SlippageSensitivity
        evidence={base}
        snapshot={snapshot}
        busy={false}
        query={query}
        run={run}
      />
    </WorkspaceDraftScope>,
  );
  const input = screen.getByLabelText("滑点变体（1–2个整数，以逗号分隔）");
  fireEvent.change(input, { target: { value: "1,2" } });
  fireEvent.click(screen.getByRole("button", { name: "核对滑点变体" }));
  fireEvent.click(screen.getByRole("button", { name: "取消滑点变体" }));
  expect(run).not.toHaveBeenCalled();
  fireEvent.change(input, { target: { value: "1,2" } });
  fireEvent.click(screen.getByRole("button", { name: "核对滑点变体" }));
  fireEvent.click(screen.getByRole("button", { name: "确认创建滑点变体" }));
  await waitFor(() => expect(run).toHaveBeenCalledTimes(2));
  expect(run.mock.calls.map(c => c[1]?.slippage_ticks)).toEqual([1, 2]);
  fireEvent.click(screen.getByRole("button", { name: "读取滑点敏感性结果" }));
  await screen.findByRole("table", { name: "滑点敏感性结果" });
  expect(screen.getByText(/成本假设敏感性：仅滑点变化/)).toBeTruthy();
});
