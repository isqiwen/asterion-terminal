import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { afterEach, expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type {
  ResearchBatchAssessment,
  ResearchBatchAssessmentSnapshot,
  ResearchBatchAssessmentDeclaration,
  Snapshot,
  ResourceObject,
} from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { BatchAssessments } from "./BatchAssessments";
import { BatchAssessmentExport } from "./BatchAssessmentExport";
import { backtestEvidence } from "./testing/backtestEvidence";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const stores = new Set<ReturnType<typeof createWorkspaceDrafts>>();
afterEach(() => {
  for (const store of stores) store.dispose();
  stores.clear();
  vi.restoreAllMocks();
});
function setup() {
  const snapshot = readySnapshot();
  const service = snapshot.research_service!;
  service.owner_node_id = "stable-owner";
  const source = backtestEvidence("source");
  source.task.program_artifact = "c".repeat(64);
  const owner = {
    node_id: service.owner_node_id,
    service_id: service.service,
    domain: "research" as const,
  };
  const object: ResourceObject = {
    node: owner.node_id,
    service: owner.service_id,
    domain: "research",
    kind: "backtest_batch",
    id: "finite-batch",
    version: "b".repeat(64),
  };
  const account = {
    currency: "CNY",
    first_day: "2026-09-22",
    last_day: "2026-09-28",
    settlement_days: 5,
    deposit: "10000",
    closing_equity: "10000",
    fees: "0",
    net_profit: "0",
  };
  const base = {
    id: "source",
    state: "succeeded" as const,
    attempt: 1,
    retired: false,
    error: "",
    input_sha256: source.input_sha256,
    result_sha256: source.task.result_digest,
    program_artifact: source.task.program_artifact,
    risk_artifact: source.task.risk_artifact!,
    account,
  };
  const observed: ResearchBatchAssessmentSnapshot = {
    batch: {
      definition: {
        object,
        owner,
        name: "Declared three scenarios",
        purpose: "Synthetic post hoc cost range",
        limitations: "Not independent evidence",
        source: {
          ...object,
          id: "source",
          kind: "backtest_result",
          version: source.task.result_digest,
        },
        members: [1, 2, 3].map(i => ({ task_id: `member-${i}`, slippage_ticks: i })),
        run_budget: 3,
        time_budget_seconds: 30,
        source_input_sha256: source.input_sha256,
        program_artifact: base.program_artifact,
        risk_artifact: base.risk_artifact,
        data_owner: { node_id: owner.node_id, service_id: "data" },
        work_units_per_run: "5",
        recorded_at_ms: 1,
      },
      phase: "stopped",
      started_at_ms: 1,
      stopped_at_ms: 2,
      elapsed_ms: "10",
      reason: "Explicit stop",
      members: [
        { task_id: "member-1", admission: "admitted", input_sha256: "a".repeat(64), error: null },
        { task_id: "member-2", admission: "admitted", input_sha256: "b".repeat(64), error: null },
        { task_id: "member-3", admission: "pending", input_sha256: "c".repeat(64), error: null },
      ],
    },
    source: base,
    members: [
      {
        ...base,
        id: "member-1",
        input_sha256: "a".repeat(64),
        result_sha256: "e".repeat(64),
        account: { ...account, closing_equity: "10024", net_profit: "24", fees: "6" },
      },
      {
        ...base,
        id: "member-2",
        state: "failed",
        input_sha256: "b".repeat(64),
        result_sha256: null,
        account: null,
        error: "Synthetic failure, not zero profit",
      },
    ],
  };
  let saved: ResearchBatchAssessment | null = null;
  const reply = {
    preview: observed,
    preview_digest: "a".repeat(64),
    assessment: null,
    assessments: [],
    next_after: "",
  };
  const query = vi.fn<TerminalContext["query"]>(async (...args): Promise<Snapshot> => {
    const [method] = args;
    if (method === "resource.catalogue")
      return {
        ...snapshot,
        resources: {
          owner: { node: owner.node_id, service: owner.service_id, domain: "research" },
          kind: "backtest_batch",
          items: [
            {
              object,
              state: "sealed",
              name: "Declared three scenarios",
              logical_bytes: "10",
              relative_location: "tasks.sqlite",
            },
          ],
          next_after: "",
        },
      };
    if (method === "research.batch.assessment.preview")
      return { ...snapshot, research_assessment: reply };
    if (method === "task.result.read") return { ...snapshot, task_result: source };
    if (method === "research.batch.assessment.read")
      return {
        ...snapshot,
        research_assessment: { ...reply, preview: null, preview_digest: "", assessment: saved },
      };
    throw new Error("Unexpected fixture query");
  });
  const trade = vi.fn<TerminalContext["trade"]>(async (...args): Promise<void> => {
    const [method, parameters] = args;
    if (method !== "research.batch.assessment.save") throw new Error("Unexpected fixture mutation");
    const declaration = structuredClone(parameters.definition);
    saved = {
      object: {
        ...object,
        kind: "backtest_batch_assessment",
        id: declaration.id,
        version: "f".repeat(64),
      },
      declaration,
      snapshot: structuredClone(observed),
      owner,
      program_artifact: "e".repeat(64),
      recorded_at_ms: "3",
    };
  });
  const props = { snapshot, busy: false, query, trade } satisfies Pick<
    TerminalContext,
    "snapshot" | "busy" | "query" | "trade"
  >;
  const drafts = createWorkspaceDrafts();
  stores.add(drafts);
  const tree = () => (
    <WorkspaceDraftScope store={drafts} namespace="assessments">
      <BatchAssessments {...props} />
    </WorkspaceDraftScope>
  );
  return {
    props,
    source,
    observed,
    reply,
    query,
    trade,
    tree,
    record: () => saved,
    render: () => render(tree()),
  };
}
async function preview() {
  fireEvent.click(screen.getByRole("button", { name: "读取可审阅批次目录" }));
  await waitFor(() =>
    expect(
      screen.getByRole("option", { name: "finite-batch · Declared three scenarios" }),
    ).toBeTruthy(),
  );
  fireEvent.change(screen.getByLabelText("完整批次来源"), { target: { value: "finite-batch" } });
  fireEvent.click(screen.getByRole("button", { name: "读取完整批次预览" }));
  await waitFor(() =>
    expect(screen.getByRole("table", { name: "全部声明成员与原账户金额" })).toBeTruthy(),
  );
}
function declare() {
  for (const [label, value] of [
    ["审阅问题", "Support this cost range?"],
    ["审阅范围", "All declared scenarios"],
    ["人工判断理由", "Missing and adverse evidence"],
    ["选择或不选择理由", "No candidate selected"],
    ["未选成员及不利证据说明", "Failed and unknown are not zero"],
    ["所见信息与用途", "Original validation was already seen"],
  ])
    fireEvent.change(screen.getByLabelText(label), { target: { value } });
  fireEvent.change(screen.getByLabelText("明确人工判断"), { target: { value: "insufficient" } });
  fireEvent.click(
    screen.getByLabelText("已核对全批次、缺失与不利证据，明确保存人工判断；不授予交易权限"),
  );
}

test("whole manifest preserves failed and uncertain evidence and saves explicit no-selection via readback", async () => {
  const view = setup();
  const original = JSON.stringify(view.source);
  view.render();
  await preview();
  const rows = screen.getByRole("table", { name: "全部声明成员与原账户金额" });
  expect(within(rows).getAllByRole("row")).toHaveLength(5);
  const failed = within(rows).getByRole("row", { name: /member-2/ });
  expect(within(failed).getAllByText("未取得成功原结果")).toHaveLength(2);
  const unknown = within(rows).getByRole("row", { name: /member-3/ });
  expect(within(unknown).getByText("接纳回执不确定")).toBeTruthy();
  expect(within(unknown).getByText("没有可确认的原任务")).toBeTruthy();
  expect(view.trade).not.toHaveBeenCalled();
  expect(
    (screen.getByRole("button", { name: "保存固定批次审阅" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  declare();
  fireEvent.click(screen.getByRole("button", { name: "保存固定批次审阅" }));
  await waitFor(() =>
    expect(screen.getByRole("article", { name: "已保存固定批次审阅" })).toBeTruthy(),
  );
  expect(view.trade).toHaveBeenCalledTimes(1);
  const params = view.trade.mock.calls[0][1] as {
    id: string;
    definition: ResearchBatchAssessmentDeclaration;
  };
  expect(params.definition.id).toBe(params.id);
  expect(params.definition).toMatchObject({
    selected_task_id: "",
    judgment: "insufficient",
    expected_snapshot: "a".repeat(64),
    information_use: "Original validation was already seen",
  });
  expect(view.query).toHaveBeenCalledWith("research.batch.assessment.read", { id: params.id });
  expect(JSON.stringify(view.source)).toBe(original);
});

test("uncertain mutation keeps exact request and all judgment text for an explicit retry", async () => {
  const view = setup();
  view.trade.mockRejectedValueOnce(new Error("Synthetic acknowledgement unavailable"));
  view.render();
  await preview();
  declare();
  fireEvent.click(screen.getByRole("button", { name: "保存固定批次审阅" }));
  await waitFor(() => expect(view.trade).toHaveBeenCalledTimes(1));
  await waitFor(() =>
    expect(screen.getByRole("button", { name: "重试同一审阅请求" })).toBeTruthy(),
  );
  await waitFor(() =>
    expect(
      (
        screen.getByLabelText(
          "已核对全批次、缺失与不利证据，明确保存人工判断；不授予交易权限",
        ) as HTMLInputElement
      ).checked,
    ).toBe(false),
  );
  expect((screen.getByLabelText("人工判断理由") as HTMLInputElement).value).toBe(
    "Missing and adverse evidence",
  );
  fireEvent.click(
    screen.getByLabelText("已核对全批次、缺失与不利证据，明确保存人工判断；不授予交易权限"),
  );
  fireEvent.click(screen.getByRole("button", { name: "重试同一审阅请求" }));
  await waitFor(() => expect(view.trade).toHaveBeenCalledTimes(2));
  expect(view.trade.mock.calls[1]).toEqual(view.trade.mock.calls[0]);
});

test("a late preview for the previous owner never appears in the replacement workspace", async () => {
  const view = setup();
  let resolve!: (value: Snapshot) => void;
  const waiting = new Promise<Snapshot>(done => {
    resolve = done;
  });
  view.query.mockImplementation(async (...args): Promise<Snapshot> => {
    const [method] = args;
    return method === "research.batch.assessment.preview"
      ? waiting
      : method === "task.result.read"
        ? { ...view.props.snapshot, task_result: view.source }
        : {
            ...view.props.snapshot,
            resources: {
              owner: {
                node: "stable-owner",
                service: view.observed.batch.definition.object.service,
                domain: "research",
              },
              kind: "backtest_batch",
              items: [
                {
                  object: view.observed.batch.definition.object,
                  name: "Declared three scenarios",
                  state: "sealed",
                  logical_bytes: "10",
                  relative_location: "tasks.sqlite",
                },
              ],
              next_after: "",
            },
          };
  });
  const rendered = view.render();
  fireEvent.click(screen.getByRole("button", { name: "读取可审阅批次目录" }));
  await waitFor(() =>
    expect(
      screen.getByRole("option", { name: "finite-batch · Declared three scenarios" }),
    ).toBeTruthy(),
  );
  fireEvent.change(screen.getByLabelText("完整批次来源"), { target: { value: "finite-batch" } });
  fireEvent.click(screen.getByRole("button", { name: "读取完整批次预览" }));
  const next = structuredClone(view.props.snapshot);
  next.research_service!.owner_node_id = "replacement-owner";
  view.props.snapshot = next;
  rendered.rerender(view.tree());
  await act(async () => resolve({ ...next, research_assessment: view.reply }));
  expect(screen.queryByRole("table", { name: "全部声明成员与原账户金额" })).toBeNull();
  expect(view.trade).not.toHaveBeenCalled();
  expect(view.query.mock.calls.filter(([method]) => method === "task.result.read")).toHaveLength(0);
});

test("a late save acknowledgement never reads a colliding ID or confirms it under the next owner", async () => {
  const view = setup();
  let finish!: () => void;
  const pending = new Promise<void>(resolve => {
    finish = resolve;
  });
  view.trade.mockImplementationOnce(async () => pending);
  const rendered = view.render();
  await preview();
  declare();
  fireEvent.click(screen.getByRole("button", { name: "保存固定批次审阅" }));
  await waitFor(() => expect(view.trade).toHaveBeenCalledTimes(1));
  const next = structuredClone(view.props.snapshot);
  next.research_service!.owner_node_id = "replacement-owner";
  view.props.snapshot = next;
  rendered.rerender(view.tree());
  await act(async () => finish());
  expect(
    view.query.mock.calls.filter(([method]) => method === "research.batch.assessment.read"),
  ).toHaveLength(0);
  expect(screen.queryByRole("article", { name: "已保存固定批次审阅" })).toBeNull();
});

test("fixed assessment export requires explicit scope and rejects a wrong-version receipt before downloading", async () => {
  const view = setup();
  view.render();
  await preview();
  declare();
  fireEvent.click(screen.getByRole("button", { name: "保存固定批次审阅" }));
  await waitFor(() =>
    expect(screen.getByRole("article", { name: "已保存固定批次审阅" })).toBeTruthy(),
  );
  const output = within(screen.getByRole("region", { name: "固定批次审阅本地导出" }));
  expect(
    (output.getByRole("button", { name: "导出固定批次审阅" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  view.query.mockResolvedValueOnce({
    ...view.props.snapshot,
    research_export: {
      name: "wrong.assessment.html",
      bytes: 5,
      content: "wrong",
      source_version: "0".repeat(64),
      conclusion_version: "",
    },
  });
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
  fireEvent.click(
    output.getByLabelText("已核对固定审阅、原条件、人工文字及诊断范围，明确导出本地文件"),
  );
  fireEvent.click(output.getByRole("button", { name: "导出固定批次审阅" }));
  await output.findByText("固定批次审阅导出确认不匹配");
  expect(click).not.toHaveBeenCalled();
  expect(output.queryByRole("status")).toBeNull();
  expect(view.record()?.declaration.judgment).toBe("insufficient");
});

test("a late original export cannot publish a receipt for the next fixed human judgment", async () => {
  const view = setup();
  const first = view.render();
  await preview();
  declare();
  fireEvent.click(screen.getByRole("button", { name: "保存固定批次审阅" }));
  await waitFor(() =>
    expect(screen.getByRole("article", { name: "已保存固定批次审阅" })).toBeTruthy(),
  );
  const fixed = view.record();
  if (!fixed) throw new Error("Expected saved review");
  first.unmount();
  view.query.mockClear();
  Object.defineProperty(URL, "createObjectURL", {
    configurable: true,
    value: vi.fn(() => "blob:fixed-assessment"),
  });
  Object.defineProperty(URL, "revokeObjectURL", { configurable: true, value: vi.fn() });
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
  let finish!: (value: Snapshot) => void;
  view.query.mockImplementation(
    () =>
      new Promise(resolve => {
        finish = resolve;
      }),
  );
  const mounted = render(
    <BatchAssessmentExport assessment={fixed} query={view.query} busy={false} />,
  );
  fireEvent.click(
    screen.getByLabelText("已核对固定审阅、原条件、人工文字及诊断范围，明确导出本地文件"),
  );
  fireEvent.click(screen.getByRole("button", { name: "导出固定批次审阅" }));
  await waitFor(() => expect(view.query).toHaveBeenCalledTimes(1));
  const next = structuredClone(fixed);
  next.object.id = "next-assessment";
  next.object.version = "b".repeat(64);
  next.declaration.id = next.object.id;
  next.declaration.previous_id = fixed.object.id;
  next.declaration.reason = "A new explicitly appended interpretation";
  next.declaration.revision_reason = "New interpretation";
  next.recorded_at_ms = "4";
  mounted.rerender(<BatchAssessmentExport assessment={next} query={view.query} busy={false} />);
  await act(async () =>
    finish({
      ...view.props.snapshot,
      research_export: {
        name: "original.assessment.html",
        bytes: 10,
        content: "Original fixed judgment",
        source_version: fixed.object.version,
        conclusion_version: "",
      },
    }),
  );
  await waitFor(() => expect(click).toHaveBeenCalledTimes(1));
  expect(screen.queryByRole("status")).toBeNull();
  expect(
    (screen.getByRole("button", { name: "导出固定批次审阅" }) as HTMLButtonElement).disabled,
  ).toBe(true);
});
