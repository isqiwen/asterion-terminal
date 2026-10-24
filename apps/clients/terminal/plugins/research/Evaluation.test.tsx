import { act, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, expect, test, vi } from "vitest";
import type { PropsWithChildren } from "react";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import type { BacktestEvidence } from "./BacktestComparison";
import { Evaluation } from "./Evaluation";
import { plugin } from "./plugin";

registerTerminalPlugins([plugin]);
// Only fields used by the view. Calculation and full-result validation are
// tested at the native boundary; this test verifies source selection and display.
const result = {
  id: "run-1",
  kind: "backtest",
  task: {
    id: "run-1",
    kind: "backtest",
    state: "succeeded",
    instrument: "SHFE/rb",
    result_digest: "d".repeat(64),
  },
  experiment: { holdout_day: "", walk_forward: null },
  performance: {
    trading_days: 5,
    total_return: 0.02,
    max_drawdown: 0.01,
    winning_days: 0.6,
    annual_return: null,
    annual_volatility: null,
    sharpe: null,
    calmar: null,
    by_month: [],
  },
  trades: {
    count: 0,
    winning: 0,
    average_win: null,
    average_loss: null,
    payoff: null,
    gross: "0",
    fees: "0",
    net: "0",
    open_fees: "0",
    closed: [],
    open: [],
    by_contract: [],
  },
} as unknown as BacktestEvidence;

const drafts = new Set<ReturnType<typeof createWorkspaceDrafts>>();
afterEach(() => {
  for (const store of drafts) store.dispose();
  drafts.clear();
});
function mount(props: TerminalContext) {
  const store = createWorkspaceDrafts();
  drafts.add(store);
  return render(<Evaluation {...props} />, {
    wrapper: ({ children }: PropsWithChildren) => (
      <WorkspaceDraftScope store={store} namespace="evaluation">
        {children}
      </WorkspaceDraftScope>
    ),
  });
}
function context(): TerminalContext {
  const snapshot = readySnapshot();
  snapshot.research_service!.tasks = [result.task];
  return {
    snapshot,
    busy: false,
    error: "",
    marketMode: "live",
    workspaceParams: { id: "run-1", connection_id: snapshot.research_service!.connection_id },
    trade: vi.fn(),
    navigate: vi.fn(),
    openSettings: vi.fn(),
    query: vi.fn(async () => ({ ...snapshot, task_result: result })),
  };
}

test("reads the selected original result without submitting an experiment or running analytics", async () => {
  const props = context();
  mount(props);
  expect(props.query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "读取评估来源" }));
  await screen.findByText("2.00%");
  expect(props.query).toHaveBeenCalledExactlyOnceWith("task.result.read", {
    id: "run-1",
    owner: {
      kind: "research",
      node_id: props.snapshot!.research_service!.node_id,
      service: props.snapshot!.research_service!.service,
      connection_id: props.snapshot!.research_service!.connection_id,
    },
  });
  expect(screen.queryByText(/无风险利率取 0/)).toBeNull();
  expect(screen.getByText("本记录未提供明确绩效方法上下文。")).toBeTruthy();
  expect(screen.getByText(/缺项分别显示原因/)).toBeTruthy();
  expect(screen.getByText("d".repeat(64))).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "查看原始实验" }));
  expect(props.navigate).toHaveBeenCalledWith("workspace.backtest", {
    page: "task",
    params: { id: "run-1", kind: "backtest" },
  });
  expect(props.trade).not.toHaveBeenCalled();
});

test("rejects a result for a different task", async () => {
  const props = context();
  props.query = vi.fn(async () => ({
    ...readySnapshot(),
    task_result: { ...result, id: "another" },
  }));
  mount(props);
  fireEvent.click(screen.getByRole("button", { name: "读取评估来源" }));
  await screen.findByText("所选任务没有回测结果");
  expect(screen.queryByText("2.00%")).toBeNull();
});

test("switching task service clears the selection and discards an in-flight result", async () => {
  const props = context();
  let resolve!: (value: ReturnType<typeof readySnapshot>) => void;
  props.query = vi.fn(
    () =>
      new Promise<ReturnType<typeof readySnapshot>>(done => {
        resolve = done;
      }),
  );
  const { rerender } = mount(props);
  fireEvent.click(screen.getByRole("button", { name: "读取评估来源" }));
  const remote = readySnapshot();
  remote.research_service!.connection_id = "remote";
  rerender(<Evaluation {...props} snapshot={remote} />);
  await act(async () => resolve({ ...readySnapshot(), task_result: result }));
  expect((screen.getByLabelText("评估来源") as HTMLSelectElement).value).toBe("");
  expect(screen.queryByText("2.00%")).toBeNull();
  expect((screen.getByRole("button", { name: "读取评估来源" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
});
