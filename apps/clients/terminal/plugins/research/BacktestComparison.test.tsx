import { backtestEvidence as evidence } from "./testing/backtestEvidence";
import { CostSensitivity, matchesCostBaseline } from "./CostSensitivity";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { captureComparison } from "./ComparisonExport";
import { fireEvent, render, screen, within } from "@testing-library/react";
import { waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { BacktestComparison, backtestComparisonReasons } from "./BacktestComparison";
import { plugin } from "./plugin";
import { TargetRejectionEvidence } from "./ModelReplayEvidence";
registerTerminalPlugins([plugin]);

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
  expect(matchesCostBaseline(base, variant, 2)).toBe(true);
  expect(backtestComparisonReasons(base, variant)).toContain("滑点不同");
  variant.experiment.paper.deposit = "20000";
  expect(matchesCostBaseline(base, variant, 2)).toBe(false);
});
test("persistent cost batch saves without execution, retries its lost receipt and explicitly starts and stops", async () => {
  const base = evidence("baseline");
  base.experiment.series = [];
  base.task.program_artifact = "e".repeat(64);
  const snapshot = readySnapshot();
  snapshot.research_service!.owner_node_id = "node-one";
  let stored: import("../../src/bridge/client").ResearchBatchReply | undefined;
  let lost = true;
  const trade = vi.fn(async (method: string, params?: Record<string, unknown>) => {
    if (method === "research.batch.save") {
      const draft = params!.definition as import("../../src/bridge/client").ResearchBatchDraft;
      stored = {
        states: [],
        next_after: "",
        members: [],
        state: {
          definition: {
            ...draft,
            object: {
              ...draft.source,
              kind: "backtest_batch",
              id: draft.id,
              version: "b".repeat(64),
            },
            owner: {
              domain: "research",
              node_id: "node-one",
              service_id: snapshot.research_service!.service,
            },
            source_input_sha256: base.input_sha256,
            program_artifact: base.task.program_artifact!,
            risk_artifact: base.task.risk_artifact!,
            data_owner: { node_id: "node-one", service_id: "historical-data" },
            work_units_per_run: "5",
            recorded_at_ms: 1,
          },
          phase: "saved",
          started_at_ms: null,
          stopped_at_ms: null,
          elapsed_ms: null,
          reason: "",
          members: draft.members.map(member => ({
            task_id: member.task_id,
            admission: "waiting",
            input_sha256: null,
            error: null,
          })),
        },
      };
    } else if (method === "research.batch.start" && stored?.state) {
      stored.state.phase = "running";
      stored.state.started_at_ms = 2;
    } else if (method === "research.batch.stop" && stored?.state) {
      stored.state.phase = "stopped";
      stored.state.reason = String(params?.reason);
      stored.state.stopped_at_ms = 3;
      stored.state.elapsed_ms = "1";
    } else throw new Error("unexpected command");
  });
  const query = vi.fn(async (method: string, params?: Record<string, unknown>) => {
    if (params?.id !== stored?.state?.definition.object.id) throw new Error("wrong batch identity");
    if (method === "research.batch.read" && lost) {
      lost = false;
      throw new Error("batch reply lost");
    }
    return { ...snapshot, research_batch: structuredClone(stored) };
  });
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="slippage">
      <CostSensitivity
        evidence={base}
        snapshot={snapshot}
        busy={false}
        query={query}
        trade={trade}
      />
    </WorkspaceDraftScope>,
  );
  const input = screen.getByLabelText("滑点变体（1–100个整数，以逗号分隔）");
  fireEvent.change(input, { target: { value: "1,2" } });
  expect(screen.getByRole("button", { name: "核对滑点变体" }).hasAttribute("disabled")).toBe(true);
  for (const [label, value] of [
    ["批次名称", "Costs"],
    ["批次用途", "Post hoc"],
    ["批次局限", "Already observed baseline"],
    ["时间预算（秒，必填）", "10"],
  ])
    fireEvent.change(screen.getByLabelText(label), { target: { value } });
  fireEvent.change(screen.getByRole("combobox", { name: "成本情景类型" }), {
    target: { value: "fees" },
  });
  const fees = screen.getByLabelText("手续费倍率（1–100个精确小数，以逗号分隔）");
  fireEvent.change(fees, { target: { value: "1" } });
  expect(screen.getByRole("button", { name: "核对手续费变体" }).hasAttribute("disabled")).toBe(
    true,
  );
  fireEvent.change(fees, { target: { value: "1.5,2" } });
  fireEvent.click(screen.getByRole("button", { name: "核对手续费变体" }));
  expect(screen.getByLabelText("手续费变体确认").textContent).toContain("1.5");
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "取消手续费变体" }));
  fireEvent.change(screen.getByRole("combobox", { name: "成本情景类型" }), {
    target: { value: "slippage" },
  });
  fireEvent.click(screen.getByRole("button", { name: "核对滑点变体" }));
  fireEvent.click(screen.getByRole("button", { name: "取消滑点变体" }));
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "核对滑点变体" }));
  fireEvent.click(screen.getByRole("button", { name: "保存批次（不执行）" }));
  await screen.findByText("batch reply lost");
  fireEvent.click(screen.getByRole("button", { name: "确认批次保存状态" }));
  await screen.findByRole("table", { name: "批次全部成员" });
  expect(trade).toHaveBeenCalledTimes(2);
  expect(trade.mock.calls[0]).toEqual(trade.mock.calls[1]);
  expect(stored?.state?.phase).toBe("saved");
  expect(screen.getAllByText("尚无已确认任务")).toHaveLength(2);
  fireEvent.click(screen.getByRole("button", { name: "确认启动批次" }));
  await waitFor(() => expect(stored?.state?.phase).toBe("running"));
  await waitFor(() =>
    expect(screen.getByRole("button", { name: "停止批次" }).hasAttribute("disabled")).toBe(true),
  );
  fireEvent.change(screen.getByLabelText("停止原因"), {
    target: { value: "Explicit cancellation" },
  });
  fireEvent.click(screen.getByRole("button", { name: "停止批次" }));
  await screen.findByText(/Explicit cancellation/);
  expect(trade.mock.calls.map(call => call[0])).toEqual([
    "research.batch.save",
    "research.batch.save",
    "research.batch.start",
    "research.batch.stop",
  ]);
  expect(stored?.state?.members.every(member => member.admission === "waiting")).toBe(true);
});

test("different fixed settlement margin assumptions change comparison eligibility and slippage identity", () => {
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
  expect(matchesCostBaseline(a, b, 0)).toBe(true);
  b.experiment.settlement_margins = [
    { trading_day: "2026-09-28", rate: "0.16", source: "explicit test assumption" },
  ];
  expect(backtestComparisonReasons(a, b)).toContain("费用或保证金日程不同");
  expect(matchesCostBaseline(a, b, 0)).toBe(false);
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
