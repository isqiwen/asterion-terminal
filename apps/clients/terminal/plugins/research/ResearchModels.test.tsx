import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type {
  ResearchModelDraft,
  ResearchModelMapping,
  ResearchModelMappingProjection,
  ResearchModelTimePlan,
  ResearchModelTimeResult,
  ResearchModelPlan,
  TaskResult,
  TaskRecord,
} from "../../src/bridge/client";
import type { CommandParams } from "../../src/bridge/commands";
import type { TerminalContext } from "../contract";
import { ResearchModels } from "./ResearchModels";
import { ResearchModelMappings } from "./ResearchModelMappings";
import { ResearchModelTimeValidation } from "./ResearchModelTimeValidation";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
function fixture() {
  const snapshot = readySnapshot();
  const service = snapshot.research_service!;
  service.owner_node_id = "stable-research-node";
  const owner = {
    node: service.owner_node_id,
    service: service.service,
    domain: "research" as const,
  };
  const source = { ...owner, kind: "factor_result", id: "source", version: "f".repeat(64) };
  const draft: ResearchModelDraft = {
    object: { ...owner, kind: "model_draft", id: "draft", version: "d".repeat(64) },
    updated_at_ms: 1,
    specification: {
      name: "Original model",
      purpose: "Synthetic diagnostic",
      limitations: "No historical trading claim",
      source,
      method: "linear",
      fit_intercept: true,
      alpha: null,
      training_begin: 0,
      training_end: 12,
      standardize: false,
      weights: "equal",
      run_budget: 1,
      time_budget_seconds: 30,
      valid_until_ms: 4102444800000,
      mode: "post_hoc_diagnostic",
    },
  };
  const plan: ResearchModelPlan = {
    object: { ...owner, kind: "model_plan", id: "plan", version: "c".repeat(64) },
    draft_id: "draft",
    draft_digest: draft.object.version,
    specification: draft.specification,
    program_artifact: "a".repeat(64),
    recorded_at_ms: 1,
    training_samples: 8,
    source: {
      result: source,
      input_sha256: "b".repeat(64),
      input_bytes: 1000,
      attempt: 1,
      program_artifact: "a".repeat(64),
      observations: 90,
      holdout_start: 45,
      horizon: 2,
      required_history: 2,
      lookback: 2,
      price_power: 0,
      factor: "momentum",
      variable_definition: null,
      historical_availability_unknown: true,
    },
  };
  const task: TaskRecord = {
    kind: "model_training",
    model_plan: plan.object,
    id: "model-task",
    state: "succeeded",
    attempt: 1,
    completed: 106,
    total: 106,
    error: "",
    result_digest: "e".repeat(64),
    trading_day: "",
    instrument: "model",
    source_name: service.service,
    submission_sequence: 1,
    submitted_at_ms: 1,
    updated_at_ms: 2,
  };
  const report: Extract<TaskResult, { kind: "model_training" }> = {
    id: task.id,
    kind: "model_training",
    input_sha256: "b".repeat(64),
    task,
    experiment: { plan },
    result: {
      plan: plan.object,
      training_samples: 8,
      intercept: 7,
      coefficient: 11,
      residual_sum_squares: 2,
      regularization: 0,
      objective: 2,
      fitted_at_ms: 2,
      standardization: null,
      rows: Array.from({ length: 90 }, (_, i) => ({
        event_index: i,
        role: i >= 45 ? "holdout" : i < 12 ? "training" : "other_development",
        reason: i < 2 ? "warmup" : i < 10 ? "fitted" : i < 12 ? "crossed_label" : "not_training",
        prediction_state: i < 2 ? "feature_unavailable" : "predicted",
        prediction: i < 2 ? null : i === 2 ? 42 : 43,
      })),
    },
  };
  return { snapshot, draft, plan, report };
}
function mount(context: TerminalContext) {
  const store = createWorkspaceDrafts();
  const view = (value: TerminalContext) => (
    <WorkspaceDraftScope store={store} namespace="models">
      <ResearchModels {...value} />
    </WorkspaceDraftScope>
  );
  const mounted = render(view(context));
  return {
    ...mounted,
    replace: (value: TerminalContext) => mounted.rerender(view(value)),
    dispose: () => {
      mounted.unmount();
      store.dispose();
    },
  };
}
const base = { busy: false, error: "", navigate: vi.fn(), openSettings: vi.fn() };
test("uncertain model save retries its original command and never starts training", async () => {
  const { snapshot, draft } = fixture();
  let saved: ResearchModelDraft | undefined;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method !== "research.model.save") throw new Error("unexpected model mutation");
    const body = params as CommandParams["research.model.save"];
    saved = {
      object: { ...draft.object, id: body.draft_id },
      specification: Object.fromEntries(
        Object.entries(body.specification).sort(([a], [b]) => a.localeCompare(b)),
      ) as typeof body.specification,
      updated_at_ms: 1,
    };
    if (trade.mock.calls.length === 1)
      throw new Error("reply unavailable after durable registration");
  });
  const query: TerminalContext["query"] = async (...[method]) => ({
    ...snapshot,
    research_model: {
      drafts: [],
      plans: [],
      mappings: [],
      time_plans: [],
      next_after: "",
      ...(method === "research.model.read" ? { draft: saved } : {}),
    },
  });
  const mounted = mount({ ...base, snapshot, query, trade });
  expect((screen.getByLabelText("回归方法") as HTMLSelectElement).value).toBe("unspecified");
  expect((screen.getByLabelText("训练次数预算") as HTMLInputElement).value).toBe("");
  expect((screen.getByLabelText("训练时间预算（秒）") as HTMLInputElement).value).toBe("");
  fireEvent.change(screen.getByLabelText("模型名称"), {
    target: { value: "Incomplete but saved" },
  });
  fireEvent.click(screen.getByRole("button", { name: "保存模型草稿" }));
  await waitFor(() => expect(screen.getByRole("alert")).toBeTruthy());
  fireEvent.click(screen.getByRole("button", { name: "保存模型草稿" }));
  await waitFor(() => expect(screen.getByText(/已保存原草稿/)).toBeTruthy());
  expect(trade).toHaveBeenCalledTimes(2);
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  expect(
    (screen.getByRole("button", { name: "固定模型训练方案" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect((screen.getByLabelText("训练样本标准化（n−1）") as HTMLInputElement).checked).toBe(false);
  expect(trade.mock.calls.every(call => call[0] === "research.model.save")).toBe(true);
  mounted.dispose();
});
test("fixed model training retries one task ID and displays only original predictions with null preserved", async () => {
  const { snapshot, plan, report } = fixture();
  const trade = vi.fn(async (...[method]: Parameters<TerminalContext["trade"]>) => {
    if (method !== "research.model.train") throw new Error("unexpected model mutation");
    if (trade.mock.calls.length === 1)
      throw new Error("reply unavailable after durable registration");
  });
  const query: TerminalContext["query"] = async (...[method]) => ({
    ...snapshot,
    research_model: {
      drafts: [],
      plans: [plan],
      mappings: [],
      time_plans: [],
      next_after: "",
      ...(method === "research.model.read" ? { plan } : {}),
    },
    ...(method === "task.result.read" ? { task_result: report } : {}),
  });
  const mounted = mount({ ...base, snapshot, query, trade });
  fireEvent.click(await screen.findByRole("button", { name: "Original model · plan" }));
  await screen.findByRole("region", { name: "固定模型训练方案" });
  fireEvent.change(screen.getByLabelText("模型训练任务ID"), { target: { value: report.id } });
  fireEvent.click(screen.getByRole("button", { name: "显式接纳训练任务" }));
  await waitFor(() => expect(screen.getByRole("alert")).toBeTruthy());
  fireEvent.click(screen.getByRole("button", { name: "显式接纳训练任务" }));
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(2));
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  await waitFor(() =>
    expect(
      (screen.getByRole("button", { name: "读取原模型结果" }) as HTMLButtonElement).disabled,
    ).toBe(false),
  );
  fireEvent.click(screen.getByRole("button", { name: "读取原模型结果" }));
  const result = await screen.findByRole("region", { name: "原模型训练结果" });
  const originalRow = within(result).getAllByRole("row")[3];
  expect(within(originalRow).getAllByRole("cell")[3].textContent).toBe("42");
  expect(within(result).getAllByText("不可用")).toHaveLength(2);
  expect(within(result).getByText(/系数.*11/)).toBeTruthy();
  mounted.dispose();
});
test("a later Research owner cannot consume the former owner's pending model result", async () => {
  const { snapshot, plan, report } = fixture();
  let release: ((reply: NonNullable<TerminalContext["snapshot"]>) => void) | undefined;
  const query: TerminalContext["query"] = async (...[method]) => {
    if (method === "task.result.read")
      return new Promise(resolve => {
        release = resolve;
      });
    return {
      ...snapshot,
      research_model: {
        drafts: [],
        plans: [plan],
        mappings: [],
        time_plans: [],
        next_after: "",
        ...(method === "research.model.read" ? { plan } : {}),
      },
    };
  };
  const context: TerminalContext = { ...base, snapshot, query, trade: vi.fn(async () => {}) };
  const mounted = mount(context);
  fireEvent.click(await screen.findByRole("button", { name: "Original model · plan" }));
  await screen.findByRole("region", { name: "固定模型训练方案" });
  fireEvent.click(screen.getByRole("button", { name: "读取原模型结果" }));
  await waitFor(() => expect(release).toBeTypeOf("function"));
  mounted.replace({
    ...context,
    snapshot: {
      ...snapshot,
      research_service: { ...snapshot.research_service!, service: "other-owner" },
    },
  });
  release!({ ...snapshot, task_result: report });
  await waitFor(() => expect(screen.queryByRole("region", { name: "原模型训练结果" })).toBeNull());
  mounted.dispose();
});

test("fixing a revised model draft creates a distinct immutable plan without starting tasks", async () => {
  const { snapshot, draft, plan } = fixture();
  let saved = draft;
  let fixed: ResearchModelPlan | undefined;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method === "research.model.save") {
      const body = params as CommandParams["research.model.save"];
      saved = {
        object: { ...draft.object, id: body.draft_id, version: "b".repeat(64) },
        specification: body.specification,
        updated_at_ms: 2,
      };
    } else if (method === "research.model.fix") {
      const body = params as CommandParams["research.model.fix"];
      fixed = {
        ...plan,
        object: { ...plan.object, id: body.plan_id },
        draft_id: body.draft_id,
        draft_digest: body.expected_digest,
        specification: saved.specification,
      };
    } else throw new Error("unexpected training or mutation");
  });
  const query: TerminalContext["query"] = async (...[method, params]) => {
    const kind =
      method === "research.model.read"
        ? (params as CommandParams["research.model.read"]).kind
        : undefined;
    return {
      ...snapshot,
      research_model: {
        drafts: [saved],
        plans: fixed ? [fixed] : [],
        mappings: [],
        time_plans: [],
        next_after: "",
        ...(kind === "draft" ? { draft: saved } : kind === "plan" ? { plan: fixed } : {}),
      },
    };
  };
  const mounted = mount({ ...base, snapshot, query, trade });
  fireEvent.click(await screen.findByRole("button", { name: "Original model · draft" }));
  const fix = () => screen.getByRole("button", { name: "固定模型训练方案" }) as HTMLButtonElement;
  await waitFor(() => expect(fix().disabled).toBe(false));
  fireEvent.click(fix());
  await screen.findByRole("region", { name: "固定模型训练方案" });
  await waitFor(() => expect(fix().disabled).toBe(false));
  fireEvent.change(screen.getByLabelText("模型名称"), { target: { value: "Revised model" } });
  expect(fix().disabled).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "保存模型草稿" }));
  await waitFor(() => expect(fix().disabled).toBe(false));
  fireEvent.click(fix());
  await screen.findByRole("region", { name: "固定模型训练方案" });
  const commands = trade.mock.calls
    .filter(call => call[0] === "research.model.fix")
    .map(call => call[1] as CommandParams["research.model.fix"]);
  expect(commands).toHaveLength(2);
  expect(commands[1].plan_id).not.toBe(commands[0].plan_id);
  expect(commands[1].expected_digest).toBe("b".repeat(64));
  expect(trade.mock.calls.every(call => call[0] !== "research.model.train")).toBe(true);
  mounted.dispose();
});

test("fixing a source requires stable business identity and never substitutes the Manager connection key", async () => {
  const { snapshot } = fixture();
  snapshot.research_service!.owner_node_id = undefined;
  const query = vi.fn<TerminalContext["query"]>(async () => ({
    ...snapshot,
    research_model: { drafts: [], plans: [], mappings: [], time_plans: [], next_after: "" },
  }));
  const mounted = mount({ ...base, snapshot, query, trade: vi.fn(async () => {}) });
  fireEvent.change(screen.getByLabelText("原因子任务ID"), { target: { value: "source" } });
  fireEvent.click(screen.getByRole("button", { name: "读取并固定原因子证据" }));
  await screen.findByRole("alert");
  expect(query.mock.calls.some(call => call[0] === "task.result.read")).toBe(false);
  expect(screen.queryByText(/固定来源:.*source/)).toBeNull();
  mounted.dispose();
});

test("fixed prediction mapping retries one original command and renders server null and intent without training", async () => {
  const { snapshot, plan, report } = fixture();
  let saved: ResearchModelMapping | undefined;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method !== "research.model.mapping.save")
      throw new Error("unexpected mutation: mapping never starts training");
    const body = params as CommandParams["research.model.mapping.save"];
    saved = {
      object: {
        ...plan.object,
        kind: "model_mapping",
        id: body.mapping_id,
        version: "9".repeat(64),
      },
      specification: {
        ...body.specification,
        model: Object.fromEntries(
          Object.entries(body.specification.model).sort(([a], [b]) => a.localeCompare(b)),
        ) as typeof body.specification.model,
      },
      plan: plan.object,
      mode: "post_hoc_diagnostic",
      recorded_at_ms: 3,
      model_published_at_ms: 2,
      folds: [],
      source: {
        result: plan.source.result,
        input_sha256: plan.source.input_sha256,
        price_power: 0,
        horizon: 2,
        historical_availability_unknown: true,
      },
    };
    if (trade.mock.calls.length === 1)
      throw new Error("reply unavailable after durable registration");
  });
  const query: TerminalContext["query"] = async (...[method]) => {
    const projection: ResearchModelMappingProjection | undefined = saved
      ? {
          mapping: saved,
          total: report.result.rows.length,
          begin: 0,
          next_begin: 50,
          observed_at_ms: 4,
          scope: "post_hoc_diagnostic",
          rows: report.result.rows.slice(0, 50).map(({ event_index, ...row }) => ({
            ...row,
            index: event_index,
            intent: row.prediction === null ? "prediction_unavailable" : "long_direction",
          })),
        }
      : undefined;
    return {
      ...snapshot,
      research_model: {
        drafts: [],
        plans: [],
        mappings: saved ? [saved] : [],
        time_plans: [],
        next_after: "",
        ...(method === "research.model.read" && saved ? { mapping: saved } : {}),
        ...(method === "research.model.mapping.preview" && projection ? { projection } : {}),
      },
    };
  };
  const store = createWorkspaceDrafts();
  const mounted = render(
    <WorkspaceDraftScope store={store} namespace="model-mapping">
      <ResearchModelMappings
        kind="model_training"
        snapshot={snapshot}
        query={query}
        trade={trade}
        busy={false}
        model={report}
      />
    </WorkspaceDraftScope>,
  );
  fireEvent.change(screen.getByLabelText("映射名称"), { target: { value: "Original mapping" } });
  fireEvent.change(screen.getByLabelText("映射用途"), {
    target: { value: "Synthetic diagnostic directions" },
  });
  fireEvent.change(screen.getByLabelText("做空阈值（收益比率）"), { target: { value: "0.4" } });
  fireEvent.change(screen.getByLabelText("做多阈值（收益比率）"), { target: { value: "0.6" } });
  fireEvent.click(screen.getByLabelText("中间区间无新方向意图，不表示平仓"));
  fireEvent.change(screen.getByLabelText("映射适用截止（UTC）"), {
    target: { value: "2099-01-01T00:00" },
  });
  const button = screen.getByRole("button", { name: /^固定预测映射$/ });
  expect((button as HTMLButtonElement).disabled).toBe(false);
  fireEvent.click(button);
  await screen.findByRole("alert");
  fireEvent.click(button);
  await screen.findByRole("region", { name: "已固定预测契约" });
  expect(trade).toHaveBeenCalledTimes(2);
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  expect((button as HTMLButtonElement).disabled).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "读取原行诊断映射" }));
  const mapped = await screen.findByRole("region", { name: "原行诊断映射" });
  expect(within(mapped).getAllByText("不可用")).toHaveLength(2);
  expect(within(mapped).getAllByText("预测不可用")).toHaveLength(2);
  const originalRow = within(mapped).getAllByRole("row")[3];
  expect(within(originalRow).getAllByRole("cell")[2].textContent).toBe("42");
  expect(within(originalRow).getAllByRole("cell")[3].textContent).toBe("做多方向意图");
  expect(within(mapped).getByText(/历史可得时间未知/)).toBeTruthy();
  mounted.unmount();
  store.dispose();
});

test("explicit chronological plan preserves unknown save identity and reads original fold roles without implicit training", async () => {
  const { snapshot, plan } = fixture();
  let saved: ResearchModelTimePlan | undefined;
  let produced: Extract<TaskResult, { kind: "model_time_validation" }> | undefined;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method === "research.model.time.save") {
      const body = params as CommandParams["research.model.time.save"];
      saved = {
        object: {
          ...plan.object,
          kind: "model_time_plan",
          id: body.plan_id,
          version: "8".repeat(64),
        },
        specification: body.specification,
        recipe: plan,
        training_counts: [8, 10],
        boundaries: body.specification.folds.flatMap(fold =>
          [fold.training_begin, fold.training_end, fold.validation_begin, fold.validation_end].map(
            event_index => ({ event_index, trading_day: "2026-09-25", timestamp_ns: null }),
          ),
        ),
        recorded_at_ms: 3,
        mode: "post_hoc_diagnostic",
      };
      if (trade.mock.calls.length === 1)
        throw new Error("reply unavailable after durable registration");
      return;
    }
    if (method !== "research.model.time.train" || !saved)
      throw new Error("unexpected time validation mutation");
    const body = params as CommandParams["research.model.time.train"];
    const task: TaskRecord = {
      kind: "model_time_validation",
      id: body.id,
      model_time_plan: saved.object,
      state: "succeeded",
      attempt: 1,
      completed: 2,
      total: 2,
      error: "",
      result_digest: "7".repeat(64),
      trading_day: "",
      instrument: "Chronology",
      source_name: "research",
      submission_sequence: 2,
      submitted_at_ms: 4,
      updated_at_ms: 5,
      program_artifact: plan.program_artifact,
    };
    snapshot.research_service!.tasks.push(task);
    const result: ResearchModelTimeResult = {
      plan: saved.object,
      computed_at_ms: 5,
      scope: "development_validation",
      historical_availability_unknown: true,
      folds: saved.specification.folds.map((scope, i) => ({
        scope,
        training_members: Array.from({ length: i ? 10 : 8 }, (_, j) => j + (i ? 8 : 2)),
        intercept: 1,
        coefficient: 2,
        training_residual_sum_squares: 0,
        regularization: 0,
        training_objective: 0,
        standardization: null,
        evaluated: i ? 6 : 5,
        validation_residual_sum_squares: 0,
        validation_mean_squared_error: 0,
        predictions: Array.from({ length: 8 }, (_, j) => ({
          event_index: scope.validation_begin + j,
          prediction: i === 0 && j === 0 ? null : j === 1 ? 0 : j / 10,
          exclusion:
            i === 0 && j === 0 ? "feature_unavailable" : j >= 6 ? "crossed_label" : "included",
        })),
      })),
    };
    produced = {
      id: body.id,
      kind: "model_time_validation",
      input_sha256: "6".repeat(64),
      task,
      experiment: { plan: saved },
      result,
    };
  });
  const query: TerminalContext["query"] = async (...[method]) => ({
    ...snapshot,
    research_model: {
      drafts: [],
      plans: [],
      mappings: [],
      time_plans: saved ? [saved] : [],
      next_after: "",
      ...(method === "research.model.read" && saved ? { time_plan: saved } : {}),
    },
    ...(method === "task.result.read" && produced ? { task_result: produced } : {}),
  });
  const store = createWorkspaceDrafts();
  const mounted = render(
    <WorkspaceDraftScope store={store} namespace="time">
      <ResearchModelTimeValidation
        snapshot={snapshot}
        query={query}
        trade={trade}
        busy={false}
        recipe={plan}
      />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(screen.getByRole("button", { name: "从当前固定方案建立时间验证" }));
  fireEvent.change(screen.getByLabelText("时间验证名称"), {
    target: { value: "Chronological development" },
  });
  fireEvent.change(screen.getByLabelText("时间验证用途"), {
    target: { value: "Synthetic cutoff audit" },
  });
  fireEvent.change(screen.getByLabelText("时间验证限制说明"), {
    target: { value: "Historical availability unknown, no trading authority" },
  });
  fireEvent.change(screen.getByRole("spinbutton", { name: "验证起始索引 1" }), {
    target: { value: "12" },
  });
  fireEvent.change(screen.getByRole("spinbutton", { name: "验证截止索引 1" }), {
    target: { value: "20" },
  });
  fireEvent.click(screen.getByRole("button", { name: "添加显式折" }));
  for (const [label, value] of [
    ["训练起始索引 2", "8"],
    ["训练截止索引 2", "20"],
    ["验证起始索引 2", "20"],
    ["验证截止索引 2", "28"],
  ])
    fireEvent.change(screen.getByRole("spinbutton", { name: label }), { target: { value } });
  fireEvent.change(screen.getByLabelText("逐折拟合次数预算"), { target: { value: "2" } });
  fireEvent.change(screen.getByLabelText("逐折任务时间预算（秒）"), { target: { value: "30" } });
  fireEvent.change(screen.getByLabelText("时间验证截止（UTC）"), {
    target: { value: "2099-01-01T00:00" },
  });
  const save = screen.getByRole("button", { name: "固定逐折计划（不执行）" });
  expect((save as HTMLButtonElement).disabled).toBe(false);
  fireEvent.click(save);
  await screen.findByRole("alert");
  fireEvent.click(save);
  const confirmation = await screen.findByRole("region", { name: "固定逐折确认" });
  expect(trade).toHaveBeenCalledTimes(2);
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  expect(snapshot.research_service!.tasks.some(task => task.kind === "model_time_validation")).toBe(
    false,
  );
  expect(within(confirmation).getAllByRole("row")[1].textContent).toContain("8");
  expect(within(confirmation).getAllByRole("row")[2].textContent).toContain("10");
  expect(screen.getAllByText(/来源未提供UTC时间/)).toHaveLength(8);
  expect(screen.getAllByText(/原交易日: 2026-09-25/)).toHaveLength(8);
  fireEvent.change(screen.getByLabelText("逐折任务ID"), { target: { value: "time-task" } });
  fireEvent.click(screen.getByRole("button", { name: "显式接纳逐折任务" }));
  await waitFor(() =>
    expect(
      (screen.getByRole("button", { name: "读取原逐折报告" }) as HTMLButtonElement).disabled,
    ).toBe(false),
  );
  fireEvent.click(screen.getByRole("button", { name: "读取原逐折报告" }));
  const report = await screen.findByRole("region", { name: "原逐折模型报告" });
  expect(within(report).getAllByRole("row")[1].textContent).toContain("不可用");
  expect(within(within(report).getAllByRole("row")[2]).getAllByRole("cell")[1].textContent).toBe(
    "0",
  );
  expect(within(report).getAllByText("标签跨越验证截止")).toHaveLength(2);
  fireEvent.change(screen.getByRole("combobox", { name: "查看原折" }), { target: { value: "1" } });
  expect(within(within(report).getAllByRole("row")[1]).getAllByRole("cell")[0].textContent).toBe(
    "20",
  );
  expect(trade.mock.calls[2][0]).toBe("research.model.time.train");
  expect(within(report).getByText(/历史可得时间未知/)).toBeTruthy();
  mounted.unmount();
  store.dispose();
});
