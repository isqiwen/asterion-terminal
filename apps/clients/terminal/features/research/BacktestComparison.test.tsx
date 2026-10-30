import { backtestEvidence as evidence } from "./testing/backtestEvidence";
import { captureComparison } from "./ComparisonExport";
import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { readySnapshot } from "../../src/testing/core";
import { BacktestComparison, backtestComparisonReasons } from "./BacktestComparison";
import { feature } from "./feature";
import { TargetRejectionEvidence } from "./TargetRejectionEvidence";
registerTerminalFeatures([feature]);

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
  snapshot.research_service!.tasks = [a.task, b.task];
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
  b.experiment.paper.contracts[0].slippage_ticks = 1;
  b.experiment.series![0][0].factor = "2";
  b.experiment.holdout_day = "2026-09-24";
  b.experiment.dataset_revision = "e".repeat(64);
  b.experiment.paper.contracts[0].dataset.daily.snapshot = "e".repeat(64);
  const reasons = backtestComparisonReasons(a, b);
  expect(reasons).toEqual(
    expect.arrayContaining([
      "费用或保证金日程不同",
      "风险限额不同",
      "滑点不同",
      "换月日程或调整因子不同",
      "训练或验证边界不同",
      "固定数据内容不同",
      "固定快照不同",
    ]),
  );
  const snapshot = readySnapshot();
  snapshot.research_service!.tasks = [a.task, b.task];
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
  b.result.folds = [
    {
      first_day: "2026-09-24",
      last_day: "2026-09-24",
      training_first_day: "2026-09-22",
      training_last_day: "2026-09-23",
      training_days: 2,
      validation_days: 1,
      candidates: [],
      selected: null,
    },
  ];
  expect(backtestComparisonReasons(a, b)).toEqual(expect.arrayContaining(["训练或验证边界不同"]));
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
    {
      first_day: "2026-09-24",
      last_day: "2026-09-24",
      training_first_day: "2026-09-22",
      training_last_day: "2026-09-23",
      training_days: 2,
      validation_days: 1,
      candidates: [],
      selected: 0,
    },
    {
      first_day: "2026-09-25",
      last_day: "2026-09-25",
      training_first_day: "2026-09-23",
      training_last_day: "2026-09-24",
      training_days: 2,
      validation_days: 1,
      candidates: [],
      selected: 1,
    },
  ];
  b.performance.out_of_sample = { ...b.performance, total_return: 0.2 };
  const snapshot = readySnapshot();
  snapshot.research_service!.tasks = [a.task, b.task];
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

test("different fixed settlement margin assumptions change comparison eligibility", () => {
  const a = evidence("before");
  a.experiment.series = [];
  const b = structuredClone(a);
  b.id = b.task.id = "after";
  b.task.metadata = {
    revision: 1,
    name: "",
    note: "",
    source_id: a.id,
    source_instance: "task",
    source_result_digest: a.task.result_digest,
    data_choice: "fixed",
    snapshot: "",
  };
  b.experiment.settlement_margins = [
    { trading_day: "2026-09-28", rate: "0.16", source: "explicit test assumption" },
  ];
  expect(backtestComparisonReasons(a, b)).toContain("费用或保证金日程不同");
});

test("ordinary simulation keeps insufficient-funds rejection evidence without inventing a model", () => {
  const source = evidence("funding-rejected");
  source.result.target_rejections = [
    {
      intent_id: "strategy.3",
      source_index: 3,
      boundary_ns: "1790000000000000000",
      reason: "insufficient_funds",
      target: "1",
    },
  ];
  render(<TargetRejectionEvidence evidence={source} />);
  const table = screen.getByRole("table", { name: "模拟风控拒单原件" });
  expect(table.textContent).toContain("保证金及手续费所需可用资金不足");
  expect(table.textContent).toContain("strategy.3");
  expect(table.textContent).toContain("3");
});
