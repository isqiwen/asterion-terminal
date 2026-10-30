import { captureComparison } from "./ComparisonExport";
import { fireEvent, render, screen, within, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { setLocale } from "../../src/i18n";
import { readySnapshot } from "../../src/testing/core";
import { FactorComparison, factorComparisonReasons, type FactorEvidence } from "./FactorComparison";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
function evidence(id: string): FactorEvidence {
  return {
    id,
    kind: "factor",
    task: {
      id,
      kind: "factor",
      state: "succeeded",
      attempt: 1,
      completed: 40,
      total: 40,
      error: "",
      result_digest: "d".repeat(64),
      trading_day: "",
      instrument: "SHFE/rb",
      source_name: "old",
      submission_sequence: 1,
      submitted_at_ms: 1,
      updated_at_ms: 1,
    },
    experiment: {
      dataset_revision: "a".repeat(64),
      factor: "momentum",
      lookbacks: [2],
      horizon: 1,
      evaluation: { mode: "full_sample" },
      liquidity: null,
      series: [
        {
          kind: "table",
          data: {
            instrument: "SHFE/rb",
            snapshot: "b".repeat(64),
            daily_source: "fixture.daily",
            calendar_source: "fixture.calendar",
            begin_day: "2026-01-01",
            end_day: "2026-03-01",
            acquired_at_ns: "1",
            months: [],
            rolls: [],
            terms: 0,
            count: 40,
          },
        },
      ],
    },
    result: {
      dataset_revision: "a".repeat(64),
      lookback: 2,
      horizon: 1,
      input_count: 40,
      purged_count: 0,
      evaluation_warmup: 2,
      selection_rule: "fixed",
      folds: [],
      candidates: [],
      memberships: [],
      samples: [],
      cross_sections: [],
      partitions: [
        {
          name: "full_sample",
          begin_index: 0,
          end_index: 40,
          sample_count: 37,
          pearson: 0.5,
          spearman: 0,
          spearman_ratio: null,
          pearson_ratio: null,
          spreads: [],
        },
      ],
    },
  };
}
function validationEvidence(id: string, mode: "holdout" | "walk_forward"): FactorEvidence {
  const value = evidence(id);
  const partition = value.result.partitions[0];
  const sample = (event_index: number) => ({
    event_index,
    observed: new Date(Date.UTC(2026, 0, event_index + 1)).toISOString().slice(0, 10),
    label: new Date(Date.UTC(2026, 0, event_index + 2)).toISOString().slice(0, 10),
    value: 0,
    forward_return: 0,
  });
  if (mode === "holdout") {
    value.experiment.evaluation = { mode, split_index: 20 };
    value.result.partitions = [
      { ...partition, name: "development", end_index: 20, sample_count: 17 },
      { ...partition, name: "holdout", begin_index: 20, sample_count: 19, pearson: null },
    ];
    value.result.samples = [
      ...Array.from({ length: 17 }, (_, i) => sample(i + 2)),
      ...Array.from({ length: 19 }, (_, i) => sample(i + 20)),
    ];
    value.result.purged_count = 1;
  } else {
    value.experiment.evaluation = { mode, training_events: 20, validation_events: 10 };
    value.result.selection_rule = "rolling_fixed";
    value.result.lookback = 0;
    value.result.partitions = [];
    value.result.folds = [0, 10].map(training_begin => ({
      training_begin,
      training_end: training_begin + 20,
      validation_end: training_begin + 30,
      lookback: 2,
      candidates: [],
      development: {
        ...partition,
        name: "development",
        begin_index: training_begin,
        end_index: training_begin + 20,
        sample_count: 17,
      },
      holdout: {
        ...partition,
        name: "holdout",
        begin_index: training_begin + 20,
        end_index: training_begin + 30,
        sample_count: 9,
      },
    }));
    value.result.samples = value.result.folds.flatMap(f =>
      Array.from({ length: 9 }, (_, i) => sample(f.training_end + i)),
    );
    value.result.purged_count = 2;
  }
  return value;
}

async function compareEvidence(values: FactorEvidence[], english = false) {
  const snapshot = readySnapshot();
  snapshot.research_service!.tasks = values.map(v => v.task);
  const query = vi.fn(async (_method: string, params?: Record<string, unknown>) => ({
    ...snapshot,
    task_result: values.find(v => v.id === params?.id)!,
  }));
  render(<FactorComparison snapshot={snapshot} busy={false} query={query} />);
  screen.getAllByRole("checkbox").forEach(box => fireEvent.click(box));
  fireEvent.click(
    screen.getByRole("button", { name: english ? "Read and Compare" : "读取并比较" }),
  );
  const table = await screen.findByRole("table", {
    name: english ? "Factor Experiment Comparison" : "因子实验对照",
  });
  return {
    table,
    query,
    matrix: captureComparison(
      table as HTMLTableElement,
      values,
      values.map(v => factorComparisonReasons(values[0], v)),
    ),
  };
}

test("holdout comparison and export keep development and holdout metrics separate", async () => {
  const a = validationEvidence("a", "holdout"),
    b = validationEvidence("b", "holdout");
  b.result.partitions[1].pearson = 0;
  const original = JSON.stringify([a, b]);
  const { matrix, query } = await compareEvidence([a, b]);
  expect(matrix.columns.map(v => v.methodology)).toEqual(["comparable", "comparable"]);
  const development = matrix.rows.find(row => row.label === "前段")!.cells[0];
  const holdout = matrix.rows.find(row => row.label === "留出段")!.cells;
  expect(development.metrics).toContainEqual({
    label: "有效样本 / 截面",
    text: "17",
    state: "defined",
  });
  expect(holdout[0].metrics).toContainEqual({
    label: "Pearson / IC 均值",
    text: "未定义",
    state: "undefined",
  });
  expect(holdout[1].metrics).toContainEqual({
    label: "Pearson / IC 均值",
    text: "0.0000",
    state: "defined",
  });
  expect(holdout[0].metrics).toContainEqual({
    label: "实际评价日期（观测至标签）",
    text: "2026-01-21 – 2026-02-09",
    state: "display_only",
  });
  expect(matrix.rows.some(row => row.label === "全样本")).toBe(false);
  expect(query.mock.calls.map(call => call[0])).toEqual(["task.result.read", "task.result.read"]);
  expect(JSON.stringify([a, b])).toBe(original);
});

test("walk-forward comparison uses each validation fold and its selected window, without pooling", async () => {
  const a = validationEvidence("a", "walk_forward"),
    b = validationEvidence("b", "walk_forward");
  b.experiment.lookbacks = [1, 2];
  b.result.selection_rule = "rolling_development_abs_spearman";
  b.result.folds[1].lookback = 1;
  b.result.folds[1].holdout.pearson = -0.25;
  const { matrix } = await compareEvidence([a, b]);
  expect(matrix.columns[1].methodology).toBe("comparable");
  const first = matrix.rows.find(row => row.label === "第 1 轮验证")!.cells[0].metrics;
  const second = matrix.rows.find(row => row.label === "第 2 轮验证")!.cells[1].metrics;
  expect(first).toContainEqual({ label: "训练观测区间", text: "1–20", state: "display_only" });
  expect(first).toContainEqual({ label: "评价观测区间", text: "21–30", state: "display_only" });
  expect(second).toContainEqual({ label: "实际窗口", text: "1", state: "display_only" });
  expect(second).toContainEqual({ label: "Pearson / IC 均值", text: "-0.2500", state: "defined" });
  expect(second).toContainEqual({
    label: "实际评价日期（观测至标签）",
    text: "2026-01-31 – 2026-02-09",
    state: "display_only",
  });
  expect(matrix.rows.some(row => ["全样本", "前段", "留出段"].includes(row.label))).toBe(false);
  expect(matrix.rows.find(row => row.label === "实际窗口")!.cells[0].text).toBe("按轮次列示");
});

test("validation methodology requires fixed and actual boundaries, warm-up and observation evidence", () => {
  const a = validationEvidence("a", "holdout"),
    b = structuredClone(a);
  b.experiment.evaluation = { mode: "holdout", split_index: 21 };
  expect(factorComparisonReasons(a, b)).toContain("训练或验证边界不同");
  const rolling = validationEvidence("r", "walk_forward"),
    changed = structuredClone(rolling);
  changed.result.folds[1].training_begin++;
  expect(factorComparisonReasons(rolling, changed)).toContain("训练或验证边界不同");
  changed.result.folds[1].training_begin--;
  changed.result.evaluation_warmup++;
  expect(factorComparisonReasons(rolling, changed)).toEqual(["评价预热不同"]);
  changed.result.evaluation_warmup--;
  changed.result.samples.pop();
  expect(factorComparisonReasons(rolling, changed)).toEqual(["实际评价观测不同"]);
});

test("mixed evaluation modes expose inapplicable segments and empty cross sections in English exports", async () => {
  setLocale("en-US");
  const full = evidence("full"),
    holdout = validationEvidence("holdout", "holdout"),
    rolling = validationEvidence("rolling", "walk_forward");
  holdout.experiment.series.push(
    structuredClone(holdout.experiment.series[0]),
    structuredClone(holdout.experiment.series[0]),
  );
  holdout.result.cross_sections = holdout.result.samples.map(row => ({
    event_index: row.event_index,
    observed: row.observed,
    label: row.label,
    pearson: null,
    spearman: null,
    series: 0,
    members: [],
  }));
  holdout.result.samples = [];
  for (const p of holdout.result.partitions) {
    p.pearson = p.spearman = null;
    p.spreads = [{ highest: 0, lowest: 0, observations: 0 }];
  }
  const { matrix } = await compareEvidence([full, holdout, rolling], true);
  expect(matrix.columns[1].reasons).toContain("Evaluation modes differ");
  const validation = matrix.rows.find(row => row.label === "Validation Fold 1")!;
  expect(validation.cells.map(cell => cell.state)).toEqual([
    "not_applicable",
    "not_applicable",
    "metric_group",
  ]);
  const holdoutMetrics = matrix.rows.find(row => row.label === "Holdout")!.cells[1].metrics;
  expect(holdoutMetrics.find(m => m.label === "Long minus short, one per side")?.state).toBe(
    "no_observations",
  );
});
test("comparison labels parameter differences, preserves zero and undefined, and only reads original results", async () => {
  const a = evidence("original"),
    b = evidence("second");
  b.experiment.lookbacks = [3];
  b.result.lookback = 3;
  b.result.partitions[0].pearson = null;
  const sample = {
    event_index: 3,
    observed: "2026-01-04",
    label: "2026-01-05",
    value: 0,
    forward_return: 0,
  };
  a.result.samples = [
    { ...sample, event_index: 2, observed: "2026-01-03", label: "2026-01-04" },
    sample,
  ];
  b.result.samples = [sample];
  const original = JSON.stringify([a, b]);
  const snapshot = readySnapshot();
  snapshot.research_service!.tasks = [a.task, b.task];
  const query = vi.fn(async (_method: string, params?: Record<string, unknown>) => ({
    ...snapshot,
    task_result: params?.id === a.id ? a : b,
  }));
  render(<FactorComparison snapshot={snapshot} busy={false} query={query} />);
  const create = screen.getByRole("button", { name: "读取并比较" });
  expect((create as HTMLButtonElement).disabled).toBe(true);
  screen.getAllByRole("checkbox").forEach(box => fireEvent.click(box));
  fireEvent.click(create);
  const table = await screen.findByRole("table", { name: "因子实验对照" });
  const row = (label: string) =>
    within(table).getByRole("rowheader", { name: label }).closest("tr")!;
  expect(row("实际窗口").textContent).toContain("3 · 不同");
  expect(row("全样本").textContent).toContain("未定义");
  expect(row("全样本").textContent).toContain("0.0000");
  expect(row("固定快照").textContent).toContain("b".repeat(64));
  expect(row("比较口径").textContent).toContain("同口径对照");
  expect(row("比较口径").textContent).toContain("仅独立列示: 实际评价观测不同");
  expect(query.mock.calls.map(call => call[0])).toEqual(["task.result.read", "task.result.read"]);
  expect(JSON.stringify([a, b])).toBe(original);
  const matrix = captureComparison(
    table as HTMLTableElement,
    [a, b],
    [factorComparisonReasons(a, a), factorComparisonReasons(a, b)],
  );
  const metric = (label: string) => matrix.rows.find(row => row.label === label)!;
  expect(metric("比较口径").cells[1].text).toContain("实际评价观测不同");
  const metrics = metric("全样本").cells.map(cell => cell.metrics);
  const field = (column: number, label: string) => metrics[column].find(v => v.label === label)!;
  expect(field(0, "Pearson / IC 均值").state).toBe("defined");
  expect(field(1, "Pearson / IC 均值").state).toBe("undefined");
  expect(field(0, "Spearman / 秩 IC 均值")).toMatchObject({ text: "0.0000", state: "defined" });
  expect(field(0, "秩 IC 信息比率").state).toBe("not_applicable");
});
test("data, eligibility, horizon differences cannot appear as comparable methodology", () => {
  const a = evidence("a"),
    b = evidence("b");
  b.experiment.dataset_revision = "c".repeat(64);
  if (b.experiment.series[0].kind === "table")
    b.experiment.series[0].data.snapshot = "e".repeat(64);
  b.experiment.horizon = 2;
  b.experiment.liquidity = { minimum_history: 2, minimum_volume: "0", minimum_open_interest: "0" };
  expect(factorComparisonReasons(a, b)).toEqual(
    expect.arrayContaining([
      "固定数据内容不同",
      "固定快照不同",
      "未来收益跨度不同",
      "资格筛选规则不同",
    ]),
  );
  a.experiment.evaluation = { mode: "holdout", split_index: 20 };
  expect(factorComparisonReasons(a, b)).toContain("评价模式不同");
});
test("comparability requires the same evaluated observations and member identities, not the same ranking", () => {
  const a = evidence("a"),
    b = evidence("b");
  const sample = {
    event_index: 2,
    observed: "2026-01-03",
    label: "2026-01-04",
    value: 0,
    forward_return: 0,
  };
  a.result.samples = [sample];
  expect(factorComparisonReasons(a, b)).toContain("实际评价观测不同");
  b.result.samples = [{ ...sample, observed: "2026-01-05" }];
  expect(factorComparisonReasons(a, b)).toContain("实际评价观测不同");
  for (const v of [a, b]) {
    v.experiment.series.push(v.experiment.series[0], v.experiment.series[0]);
    v.result.samples = [];
    v.result.cross_sections = [
      {
        event_index: 2,
        observed: sample.observed,
        label: sample.label,
        pearson: null,
        spearman: null,
        series: 2,
        members: [
          { series_index: 0, value: 0.1, forward_return: 0 },
          { series_index: 1, value: 0, forward_return: 0 },
        ],
      },
    ];
  }
  b.result.cross_sections[0].members.reverse();
  expect(factorComparisonReasons(a, b)).toEqual([]);
  b.result.cross_sections[0].members[0].series_index = 2;
  expect(factorComparisonReasons(a, b)).toEqual(["实际参与成员不同"]);
});
test("failed result reads show their error without retries or writes", async () => {
  const a = evidence("a"),
    b = evidence("legacy"),
    snapshot = readySnapshot();
  snapshot.research_service!.tasks = [a.task, b.task];
  const query = vi.fn(async (_method: string, params?: Record<string, unknown>) => {
    if (params?.id === "legacy") throw new Error("invalid factor result");
    return { ...snapshot, task_result: a };
  });
  render(<FactorComparison snapshot={snapshot} busy={false} query={query} />);
  screen.getAllByRole("checkbox").forEach(box => fireEvent.click(box));
  fireEvent.click(screen.getByRole("button", { name: "读取并比较" }));
  await waitFor(() => expect(query).toHaveBeenCalledTimes(2));
  expect(await screen.findByText("部分结果不可读，以下仅列示已读取结果。")).toBeTruthy();
  expect(screen.getByText("invalid factor result")).toBeTruthy();
  expect(screen.queryByRole("button", { name: "导出当前比较证据" })).toBeNull();
});

test("selection is limited to three experiments and can be cleared", () => {
  const snapshot = readySnapshot();
  snapshot.research_service!.tasks = ["a", "b", "c", "d"].map(id => evidence(id).task);
  render(<FactorComparison snapshot={snapshot} busy={false} query={vi.fn(async () => snapshot)} />);
  const boxes = screen.getAllByRole("checkbox") as HTMLInputElement[];
  boxes.slice(0, 3).forEach(box => fireEvent.click(box));
  expect(boxes[3].disabled).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "清空比较选择" }));
  expect(boxes.every(box => !box.checked && !box.disabled)).toBe(true);
  expect((screen.getByRole("button", { name: "读取并比较" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
});
