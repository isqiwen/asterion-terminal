import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { afterEach, expect, test, vi } from "vitest";
import type { PropsWithChildren } from "react";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { taskOwner, type TaskResult } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { EvaluationReports, type EvaluationSource } from "./EvaluationReports";
import { EvaluationExport } from "./EvaluationExport";
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
  service.owner_node_id = "stable-research-owner";
  const source: EvaluationSource = {
    object: {
      node: service.owner_node_id,
      service: service.service,
      domain: "research",
      kind: "backtest_result",
      id: "source-1",
      version: "d".repeat(64),
    },
    owner: taskOwner(service, "research"),
    program: "c".repeat(64),
    scopes: ["full_account"],
    retired: false,
  };
  const report: Extract<TaskResult, { kind: "performance_evaluation" }> = {
    id: "evaluation-1",
    kind: "performance_evaluation",
    input_sha256: "e".repeat(64),
    task: {
      id: "evaluation-1",
      kind: "performance_evaluation",
      state: "succeeded",
      attempt: 1,
      completed: 3,
      total: 3,
      error: "",
      result_digest: "f".repeat(64),
      trading_day: "",
      instrument: "source-1",
      source_name: service.service,
      submission_sequence: 2,
      submitted_at_ms: 2,
      updated_at_ms: 2,
      program_artifact: "b".repeat(64),
    },
    experiment: {
      request: {
        source: source.object,
        question: "Fixed facts, not an investment conclusion",
        scope: "full_account",
        include_daily_drawdown: false,
        include_exposure: false,
        static_stress: null,
        method: { annual_periods: null, risk_free: "unconfigured" },
      },
      source: {
        input_sha256: "a".repeat(64),
        input_bytes: "200",
        attempt: 1,
        program_artifact: source.program,
        settlements: 3,
        first_settlement: 0,
        end_settlement: 3,
      },
    },
    result: {
      daily_drawdown: null,
      exposure: null,
      static_stress: null,
      first_day: "2026-09-21",
      last_day: "2026-09-23",
      begin_ns: "1790000000000000000",
      end_ns: "1790172800000000000",
      opening_equity: "10000",
      closing_equity: "10050",
      fees: "2",
      net_profit: "50",
      currency: "CNY",
      trading_days: 3,
      complete_return_periods: 2,
      opening_is_full_period: false,
      short_calendar_span: true,
      calendar_year_days: 365.2425,
      total_return: { state: "AVAILABLE", value: 0.005 },
      max_drawdown: { state: "AVAILABLE", value: 0.02 },
      winning_days: { state: "AVAILABLE", value: 2 / 3 },
      annual_return: { state: "AVAILABLE", value: 0.1234 },
      annual_volatility: { state: "ANNUAL_PERIODS_NOT_CONFIGURED", value: null },
      sharpe: { state: "ANNUAL_PERIODS_NOT_CONFIGURED", value: null },
      calmar: { state: "AVAILABLE", value: 6.17 },
      limitations: [
        "CASH_FLOWS_NOT_EVALUATED",
        "BENCHMARK_NOT_EVALUATED",
        "RISK_AND_STRESS_NOT_EVALUATED",
        "INDEPENDENCE_NOT_ESTABLISHED",
        "HISTORICAL_AVAILABILITY_NOT_VERIFIED",
        "HUMAN_CONCLUSION_NOT_RECORDED",
      ],
    },
  };
  const trade = vi.fn<TerminalContext["trade"]>(async () => {});
  const query = vi.fn<TerminalContext["query"]>(async () => snapshot);
  const props = { snapshot, trade, query, busy: false, workspaceParams: {}, source };
  const store = createWorkspaceDrafts();
  stores.add(store);
  const wrapper = ({ children }: PropsWithChildren) => (
    <WorkspaceDraftScope store={store} namespace="fixed-evaluation">
      {children}
    </WorkspaceDraftScope>
  );
  return {
    props,
    report,
    source,
    render: () => render(<EvaluationReports {...props} />, { wrapper }),
  };
}

test("submission fixes the stable business owner and keeps the same request ID without implicit A or Rf", async () => {
  const view = setup();
  view.render();
  expect((screen.getByLabelText("年期数A（日结算）") as HTMLInputElement).value).toBe("");
  expect((screen.getByLabelText("明确采用零无风险年收益假设") as HTMLInputElement).checked).toBe(
    false,
  );
  fireEvent.change(screen.getByLabelText("评价问题与用途"), {
    target: { value: "Original account evidence" },
  });
  fireEvent.click(screen.getByRole("button", { name: "计算固定绩效评价" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(1));
  expect(view.props.trade).toHaveBeenCalledWith(
    "evaluation.submit",
    expect.objectContaining({
      owner: view.source.owner,
      source: view.source.object,
      method: { annual_periods: null, risk_free: "unconfigured" },
      include_daily_drawdown: false,
      include_exposure: false,
    }),
  );
  fireEvent.click(screen.getByRole("button", { name: "重试同一评价请求" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(2));
  expect(view.props.trade.mock.calls[1]).toEqual(view.props.trade.mock.calls[0]);
  fireEvent.click(screen.getByLabelText("保存日采样损失路径（不代表盘中最大风险）"));
  await waitFor(() =>
    expect(screen.getByRole("button", { name: "计算固定绩效评价" })).toBeTruthy(),
  );
  fireEvent.click(screen.getByRole("button", { name: "计算固定绩效评价" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(3));
  expect(view.props.trade.mock.calls[2][1]).toMatchObject({ include_daily_drawdown: true });
  expect(view.props.trade.mock.calls[2][1]).not.toEqual(view.props.trade.mock.calls[0][1]);
  const first = view.props.trade.mock.calls[0][1];
  const latest = view.props.trade.mock.calls[2][1];
  if (!first || !latest || !("id" in first) || !("id" in latest))
    throw new Error("Expected fixed evaluation submission identities");
  expect(latest.id).not.toBe(first.id);
  fireEvent.click(screen.getByLabelText("保存本范围末日敞口和资金观察（非盘中峰值）"));
  await waitFor(() =>
    expect(screen.getByRole("button", { name: "计算固定绩效评价" })).toBeTruthy(),
  );
  fireEvent.click(screen.getByRole("button", { name: "计算固定绩效评价" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(4));
  const withExposure = view.props.trade.mock.calls[3][1];
  if (!withExposure || !("id" in withExposure))
    throw new Error("Expected fixed exposure request identity");
  expect(withExposure.id).not.toBe(latest.id);
  expect(withExposure).toMatchObject({ include_daily_drawdown: true, include_exposure: true });

  expect((screen.getByLabelText("保存显式静态价格与保证金压力") as HTMLInputElement).checked).toBe(
    false,
  );
  fireEvent.click(screen.getByLabelText("保存显式静态价格与保证金压力"));
  expect(
    (screen.getByRole("button", { name: "计算固定绩效评价" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  fireEvent.change(screen.getByLabelText("压力用途与范围"), {
    target: { value: "Explicit fixed-position funding scenario" },
  });
  fireEvent.change(screen.getByLabelText("情景价格（原价格单位）"), { target: { value: "80" } });
  fireEvent.change(screen.getByLabelText("假设保证金率（比例，0.4表示40%）"), {
    target: { value: "0.4" },
  });
  expect(
    (screen.getByRole("button", { name: "计算固定绩效评价" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  fireEvent.click(
    screen.getByLabelText("明确采用原纸面资金模型：余额与权益取小，减保证金及原冻结"),
  );
  fireEvent.click(screen.getByRole("button", { name: "计算固定绩效评价" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(5));
  const withStress = view.props.trade.mock.calls[4][1];
  expect(withStress).toMatchObject({
    include_exposure: true,
    static_stress: {
      purpose: "Explicit fixed-position funding scenario",
      price: "80",
      margin_rate: "0.4",
      funds_model: "paper_balance_equity_minimum",
    },
  });
  fireEvent.change(screen.getByLabelText("情景价格（原价格单位）"), { target: { value: "-10" } });
  fireEvent.click(screen.getByRole("button", { name: "计算固定绩效评价" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(6));
  const changed = view.props.trade.mock.calls[5][1];
  if (!withStress || !changed || !("id" in withStress) || !("id" in changed))
    throw new Error("Expected stress identities");
  expect(changed.id).not.toBe(withStress.id);
  expect(changed).toMatchObject({ static_stress: { price: "-10" } });
  expect(view.props.query).not.toHaveBeenCalled();
});

test("retired formal reports remain readable and draft method changes never recalculate saved values", async () => {
  const view = setup();
  view.source.retired = true;
  view.report.task.retired = true;
  view.props.snapshot.research_service!.tasks = [view.report.task];
  view.props.query.mockResolvedValue({ ...view.props.snapshot, task_result: view.report });
  view.render();
  fireEvent.change(screen.getByLabelText("已保存绩效评价"), { target: { value: view.report.id } });
  fireEvent.click(screen.getByRole("button", { name: "读取固定绩效评价" }));
  const report = await screen.findByRole("article", { name: "固定评价报告" });
  expect(within(report).getByText("12.34%")).toBeTruthy();
  expect(within(report).getByText("b".repeat(64))).toBeTruthy();
  expect(within(report).getByText("c".repeat(64))).toBeTruthy();
  expect(report.textContent).toContain("年期数A未配置");
  fireEvent.change(screen.getByLabelText("年期数A（日结算）"), { target: { value: "252" } });
  fireEvent.click(screen.getByLabelText("明确采用零无风险年收益假设"));
  expect(within(report).getByText("12.34%")).toBeTruthy();
  expect(within(report).queryByText("明确零无风险")).toBeNull();
  expect(report.textContent).toContain("未证明独立验证资格");
  expect(
    (screen.getByRole("button", { name: "计算固定绩效评价" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(view.props.trade).not.toHaveBeenCalled();
});

test("changing the Research owner discards a late report and blocks the old captured source", async () => {
  const view = setup();
  view.props.snapshot.research_service!.tasks = [view.report.task];
  let resolve!: (value: ReturnType<typeof readySnapshot>) => void;
  view.props.query.mockImplementation(
    () =>
      new Promise(done => {
        resolve = done;
      }),
  );
  const mounted = view.render();
  fireEvent.change(screen.getByLabelText("已保存绩效评价"), { target: { value: view.report.id } });
  fireEvent.click(screen.getByRole("button", { name: "读取固定绩效评价" }));
  const other = readySnapshot();
  other.research_service!.service = "another-research";
  other.research_service!.owner_node_id = view.props.snapshot.research_service!.owner_node_id;
  mounted.rerender(<EvaluationReports {...view.props} snapshot={other} />);
  await act(async () => resolve({ ...view.props.snapshot, task_result: view.report }));
  expect(screen.queryByRole("article", { name: "固定评价报告" })).toBeNull();
  const form = screen.getByLabelText("评价问题与用途").closest("fieldset") as HTMLFieldSetElement;
  expect(form.disabled).toBe(true);
  expect(view.props.trade).not.toHaveBeenCalled();
});

test("evaluation export requires explicit fixed scope and rejects a mismatched receipt without success", async () => {
  const view = setup();
  expect(view.report.task.result_digest).not.toBe("0".repeat(64));
  view.props.query.mockResolvedValue({
    ...view.props.snapshot,
    research_export: {
      name: "wrong.html",
      bytes: 5,
      content: "wrong",
      source_version: "0".repeat(64),
      conclusion_version: "",
    },
  });
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
  render(
    <EvaluationExport
      report={view.report}
      owner={view.source.owner}
      query={view.props.query}
      busy={false}
    />,
  );
  expect(
    (screen.getByRole("button", { name: "导出固定评价报告" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  fireEvent.click(
    screen.getByLabelText("已核对固定报告、人工解释范围及全部限制，明确导出本地文件"),
  );
  fireEvent.click(screen.getByRole("button", { name: "导出固定评价报告" }));
  await screen.findByText("评价报告导出确认不匹配");
  expect(view.props.query).toHaveBeenCalledWith("evaluation.report.export", {
    id: view.report.id,
    owner: view.source.owner,
    version: view.report.task.result_digest,
    format: "html",
    conclusion_id: "",
    conclusion_version: "",
    path: "",
  });
  expect(screen.queryByRole("status")).toBeNull();
  expect(click).not.toHaveBeenCalled();
});

test("a late original export cannot publish a receipt in another Research owner scope", async () => {
  const view = setup();
  Object.defineProperty(URL, "createObjectURL", {
    configurable: true,
    value: vi.fn(() => "blob:fixed-evaluation"),
  });
  Object.defineProperty(URL, "revokeObjectURL", { configurable: true, value: vi.fn() });
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
  let complete!: (value: ReturnType<typeof readySnapshot>) => void;
  view.props.query.mockImplementation(
    () =>
      new Promise(resolve => {
        complete = resolve;
      }),
  );
  const mounted = render(
    <EvaluationExport
      report={view.report}
      owner={view.source.owner}
      query={view.props.query}
      busy={false}
    />,
  );
  fireEvent.click(
    screen.getByLabelText("已核对固定报告、人工解释范围及全部限制，明确导出本地文件"),
  );
  fireEvent.click(screen.getByRole("button", { name: "导出固定评价报告" }));
  await waitFor(() => expect(view.props.query).toHaveBeenCalledTimes(1));
  mounted.rerender(
    <EvaluationExport
      report={view.report}
      owner={{ ...view.source.owner, service: "another-research" }}
      query={view.props.query}
      busy={false}
    />,
  );
  await act(async () => {
    complete({
      ...view.props.snapshot,
      research_export: {
        name: "original.evaluation.html",
        bytes: 123,
        content: "Original fixed report",
        source_version: view.report.task.result_digest,
        conclusion_version: "",
      },
    });
  });
  await waitFor(() => expect(click).toHaveBeenCalledTimes(1));
  // The explicitly requested original download may finish; its receipt must not
  // be presented as a successful operation in the newly selected owner.
  expect(screen.queryByRole("status")).toBeNull();
  expect(
    (screen.getByRole("button", { name: "导出固定评价报告" }) as HTMLButtonElement).disabled,
  ).toBe(true);
});

test("ledger request is explicit, fixes a full-account scope and does not silently approximate holdout", async () => {
  const view = setup();
  view.source.scopes.push("holdout");
  view.render();
  const checkbox = screen.getByLabelText(
    "保存原账本归因桥接（仅完整账户、单合约日线）",
  ) as HTMLInputElement;
  expect(checkbox.checked).toBe(false);
  fireEvent.change(screen.getByLabelText("评价问题与用途"), {
    target: { value: "Original ledger checks" },
  });
  fireEvent.click(checkbox);
  fireEvent.change(screen.getByRole("combobox", { name: "评价范围" }), {
    target: { value: "holdout" },
  });
  expect(
    (screen.getByRole("button", { name: "计算固定绩效评价" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(view.props.trade).not.toHaveBeenCalled();
  fireEvent.change(screen.getByRole("combobox", { name: "评价范围" }), {
    target: { value: "full_account" },
  });
  fireEvent.click(screen.getByRole("button", { name: "计算固定绩效评价" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(1));
  expect(view.props.trade.mock.calls[0][1]).toMatchObject({
    include_ledger: true,
    scope: "full_account",
    source: view.source.object,
  });
});

test("original contract contribution request is opt-in and refuses a silently approximated partial account", async () => {
  const view = setup();
  view.source.scopes.push("holdout");
  view.render();
  const checkbox = screen.getByLabelText(
    "保存原合约贡献（仅完整账户、全部日线）",
  ) as HTMLInputElement;
  expect(checkbox.checked).toBe(false);
  fireEvent.change(screen.getByLabelText("评价问题与用途"), {
    target: { value: "Actual contract contributions" },
  });
  fireEvent.click(checkbox);
  fireEvent.change(screen.getByRole("combobox", { name: "评价范围" }), {
    target: { value: "holdout" },
  });
  expect(
    (screen.getByRole("button", { name: "计算固定绩效评价" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(view.props.trade).not.toHaveBeenCalled();
  fireEvent.change(screen.getByRole("combobox", { name: "评价范围" }), {
    target: { value: "full_account" },
  });
  fireEvent.click(screen.getByRole("button", { name: "计算固定绩效评价" }));
  await waitFor(() => expect(view.props.trade).toHaveBeenCalledTimes(1));
  expect(view.props.trade.mock.calls[0][1]).toMatchObject({
    include_contributions: true,
    scope: "full_account",
    source: view.source.object,
  });
});
