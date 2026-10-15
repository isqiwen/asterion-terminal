import { captureComparison } from "./ComparisonExport";
import { fireEvent, render, screen, within, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import {
  FactorComparison,
  factorComparisonReasons,
  factorComparisonValueState,
  type FactorEvidence,
} from "./FactorComparison";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
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
      version: 9,
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
      version: 9,
      dataset_revision: "a".repeat(64),
      engine_version: "asterion.factor.v11",
      lookback: 2,
      horizon: 1,
      input_count: 40,
      purged_count: 0,
      evaluation_warmup: 0,
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
test("comparison labels parameter differences, preserves zero and undefined, and only reads original results", async () => {
  const a = evidence("original"),
    b = evidence("second");
  b.experiment.lookbacks = [3];
  b.result.lookback = 3;
  b.result.partitions[0].pearson = null;
  const original = JSON.stringify([a, b]);
  const snapshot = readySnapshot();
  snapshot.task_service!.tasks = [a.task, b.task];
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
  expect(row("Pearson / IC 均值").textContent).toContain("未定义");
  expect(row("Spearman / 秩 IC 均值").textContent).toContain("0.0000");
  expect(row("每侧 1 个的多空差").textContent).toContain("—");
  expect(row("固定快照").textContent).toContain("b".repeat(64));
  expect(row("比较口径").textContent).toContain("同口径对照");
  expect(query.mock.calls.map(call => call[0])).toEqual(["task.result.read", "task.result.read"]);
  expect(JSON.stringify([a, b])).toBe(original);
  const matrix = captureComparison(
    table as HTMLTableElement,
    [a, b],
    [factorComparisonReasons(a, a), factorComparisonReasons(a, b)],
  );
  const metric = (label: string) => matrix.rows.find(row => row.label === label)!;
  expect(metric("Pearson / IC 均值").cells.map(cell => cell.state)).toEqual([
    "defined",
    "undefined",
  ]);
  expect(metric("Spearman / 秩 IC 均值").cells[0]).toMatchObject({
    text: "0.0000",
    state: "defined",
  });
  expect(metric("秩 IC 信息比率").cells[0].state).toBe("not_applicable");
  const missing = evidence("missing");
  missing.result.partitions = [];
  expect(factorComparisonValueState("Pearson / IC 均值", missing)).toBe("missing");
  const noObservations = evidence("no-observations");
  noObservations.experiment.series.push(
    ...noObservations.experiment.series,
    ...noObservations.experiment.series,
  );
  expect(factorComparisonValueState("每侧 1 个的多空差", noObservations)).toBe("no_observations");
});
test("data, eligibility, horizon and engine differences cannot appear as comparable methodology", () => {
  const a = evidence("a"),
    b = evidence("b");
  b.experiment.dataset_revision = "c".repeat(64);
  if (b.experiment.series[0].kind === "table")
    b.experiment.series[0].data.snapshot = "e".repeat(64);
  b.experiment.horizon = 2;
  b.experiment.liquidity = { minimum_history: 2, minimum_volume: "0", minimum_open_interest: "0" };
  b.result.engine_version = "other";
  expect(factorComparisonReasons(a, b)).toEqual(
    expect.arrayContaining([
      "固定数据内容不同",
      "固定快照不同",
      "未来收益跨度不同",
      "资格筛选规则不同",
      "输入、结果或引擎版本不同",
    ]),
  );
  a.experiment.evaluation = { mode: "holdout", split_index: 20 };
  expect(factorComparisonReasons(a, b)).toContain("仅支持全样本评价");
});
test("incompatible old result reads show their error without retries or writes", async () => {
  const a = evidence("a"),
    b = evidence("legacy"),
    snapshot = readySnapshot();
  snapshot.task_service!.tasks = [a.task, b.task];
  const query = vi.fn(async (_method: string, params?: Record<string, unknown>) => {
    if (params?.id === "legacy") throw new Error("unsupported factor input version");
    return { ...snapshot, task_result: a };
  });
  render(<FactorComparison snapshot={snapshot} busy={false} query={query} />);
  screen.getAllByRole("checkbox").forEach(box => fireEvent.click(box));
  fireEvent.click(screen.getByRole("button", { name: "读取并比较" }));
  await waitFor(() => expect(query).toHaveBeenCalledTimes(2));
  expect(await screen.findByText("部分结果不可读，以下仅列示已读取结果。")).toBeTruthy();
  expect(screen.getByText("unsupported factor input version")).toBeTruthy();
  expect(screen.queryByRole("button", { name: "导出当前比较证据" })).toBeNull();
});

test("selection is limited to three experiments and can be cleared", () => {
  const snapshot = readySnapshot();
  snapshot.task_service!.tasks = ["a", "b", "c", "d"].map(id => evidence(id).task);
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
