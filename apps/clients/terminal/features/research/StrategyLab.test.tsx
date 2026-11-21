import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { EditorView } from "codemirror";
import { beforeEach, expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import { Period } from "../../src/generated/asterion/common_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import {
  RunDetailSchema,
  RunState,
  RunSummarySchema,
  RunsSchema,
  SourceSchema,
  StrategiesSchema,
  SweepRunsSchema,
} from "../../src/generated/asterion/lab_pb";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { StrategyLab } from "./StrategyLab";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
}));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, string>) =>
    values ? `${key} ${Object.values(values).join(" ")}` : key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

beforeEach(() => localStorage.clear());

const editScope = (edit: () => void) => {
  fireEvent.click(screen.getByRole("button", { name: /^(修改|设置)回测条件$/ }));
  edit();
  fireEvent.click(screen.getByRole("button", { name: "完成" }));
};

const editor = () => EditorView.findFromDOM(document.querySelector(".cm-editor") as HTMLElement)!;

function mockRunDetail(id: string, strategy = "once.Once") {
  return create(RunDetailSchema, {
    summary: { id, strategy, state: RunState.SUCCEEDED, metrics: { total_return: 0.12 } },
    record: JSON.stringify({
      strategy: { id: strategy, commit: "abc12345", changes: "" },
      params: { fast: 8 },
      costs: { deposit: "500000", slippage_ticks: 2 },
      risk: {
        max_order_lots: "5",
        max_contract_lots: "0",
        max_gross_lots: "10",
        max_working_orders: 6,
        max_gross_weight: "1",
        max_net_weight: "1",
      },
      statistics: { annual_periods: 252, zero_risk_free: true, holdout_day: "2026-02-01" },
      engine: { version: "test" },
      data: {
        dataset_version: "a".repeat(64),
        symbols: ["SHFE/rb"],
        begin_day: "2026-01-05",
        end_day: "2026-02-13",
        bars: { period: "5m" },
      },
    }),
  } as never);
}

test("an unsaved edit is saved before the run reads it, and the run opens its report in place", async () => {
  const files: Record<string, string> = { "strategies/once.py": "LOTS = 1\n" };
  const order: string[] = [];
  let startedId = "";
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, { strategies: [{ id: "once.Once" }] }) as never;
      case "listRecentRuns":
        return create(RunsSchema, {}) as never;
      case "readSource":
        return create(SourceSchema, { content: files[operation.value.path!] }) as never;
      case "saveSource":
        files[operation.value.source!.path!] = operation.value.content!;
        order.push(`saved ${operation.value.content}`);
        return {} as never;
      case "startBacktest":
        startedId = operation.value.id!;
        order.push(`ran with ${files["strategies/once.py"]}`);
        return {} as never;
      case "getRunSummary":
        return create(RunSummarySchema, {
          id: operation.value.id,
          strategy: "once.Once",
          state: RunState.SUCCEEDED,
        }) as never;
      case "getRun":
        return mockRunDetail(operation.value.id!) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "Once" }));
  await waitFor(() => expect(editor().state.doc.toString()).toBe("LOTS = 1\n"));
  editor().dispatch({ changes: { from: 7, to: 8, insert: "2" } });
  editScope(() => {
    fireEvent.focus(screen.getByLabelText("品种"));
    fireEvent.click(screen.getByRole("option", { name: /SHFE\/rb/ }));
  });
  fireEvent.click(screen.getByRole("button", { name: "运行" }));

  // The edit was saved first and the report opens in place while keeping the config panel live.
  await screen.findByText("已保存 {files} strategies/once.py");
  expect(order).toEqual(["saved LOTS = 2\n", "ran with LOTS = 2\n"]);
  expect(await screen.findByRole("button", { name: "在回测中打开" })).toBeTruthy();
  expect(screen.getByRole("button", { name: "运行" })).toBeTruthy();

  fireEvent.click(screen.getByRole("button", { name: "在回测中打开" }));
  expect(context.navigate).toHaveBeenCalledWith("workspace.lab-runs", {
    params: { run: startedId },
  });
  fireEvent.click(screen.getByRole("button", { name: "去部署" }));
  expect(context.navigate).toHaveBeenCalledWith("workspace.trading-strategies", {
    params: { run: startedId },
  });

  // The code tab shows what was saved.
  fireEvent.click(screen.getByRole("button", { name: "代码" }));
  await waitFor(() => expect(editor().state.doc.toString()).toBe("LOTS = 2\n"));
});

test("only a successful individual run offers deployment from a restored report", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {}) as never;
      case "listRecentRuns":
        return create(RunsSchema, {}) as never;
      case "getRunSummary":
        return create(RunSummarySchema, {
          id: operation.value.id,
          strategy: "once.Once",
          state: operation.value.id === "failed" ? RunState.FAILED : RunState.SUCCEEDED,
          sweepId: operation.value.id === "scan-rolling" ? "scan" : "",
        }) as never;
      case "getRun":
        return mockRunDetail(operation.value.id!) as never;
      case "getSweep":
        return create(SweepRunsSchema, {
          runs: [{ id: "candidate", state: RunState.SUCCEEDED }],
        }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const drafts = createWorkspaceDrafts();
  const namespace = "workspace.lab-strategies";
  const reportKey = JSON.stringify([namespace, "strategy-report:/research"]);
  drafts.write(reportKey, { kind: "run", id: "failed" });
  drafts.write(JSON.stringify([namespace, "strategy-view"]), "detail");
  drafts.write(JSON.stringify([namespace, "strategy-tab"]), "result");
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={drafts} namespace={namespace}>
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  await screen.findByText("失败");
  expect(screen.queryByRole("button", { name: "去部署" })).toBeNull();
  await act(async () => drafts.write(reportKey, { kind: "run", id: "scan-rolling" }));
  await screen.findByText("已完成");
  expect(screen.queryByRole("button", { name: "去部署" })).toBeNull();
  await act(async () => drafts.write(reportKey, { kind: "sweep", id: "scan" }));
  await screen.findByRole("region", { name: "排名" });
  expect(screen.queryByRole("button", { name: "去部署" })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "查看" }));
  fireEvent.click(await screen.findByRole("button", { name: "去部署" }));
  expect(context.navigate).toHaveBeenCalledWith("workspace.trading-strategies", {
    params: { run: "candidate" },
  });
});

test("the library comes first, and a strategy without a run opens on its code", async () => {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, { strategies: [{ id: "once.Once" }] }) as never;
      case "listRecentRuns":
        return create(RunsSchema, {}) as never;
      case "readSource":
        return create(SourceSchema, { content: "x = 1\n" }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "Once" }));
  expect(await screen.findByRole("button", { name: "运行" })).toBeTruthy();
  await waitFor(() => expect(editor().state.doc.toString()).toBe("x = 1\n"));
  fireEvent.click(screen.getByRole("button", { name: "收起配置" }));
  expect(screen.queryByRole("button", { name: "运行" })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "展开配置" }));
  expect(screen.getByRole("button", { name: "运行" })).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "← 策略库" }));
  expect(screen.getByRole("table", { name: "策略库" })).toBeTruthy();
});

test("recent runs open in place and loadRun populates the form", async () => {
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          strategies: [
            {
              id: "once.Once",
              parameters: [{ name: "fast", default: { value: { case: "integer", value: 3n } } }],
            },
          ],
        }) as never;
      case "listRecentRuns":
        return create(RunsSchema, {
          runs: [
            { id: "once-2", strategy: "once.Once", label: "快线", metrics: { total_return: 0.1 } },
            { id: "other-1", strategy: "other.Other" },
          ],
        }) as never;
      case "readSource":
        return create(SourceSchema, { content: "" }) as never;
      case "getRunSummary":
        return create(RunSummarySchema, {
          id: operation.value.id,
          strategy: "once.Once",
          label: "快线",
          state: RunState.SUCCEEDED,
        }) as never;
      case "getRun":
        return mockRunDetail(operation.value.id!) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    workspaceParams: { loadRun: "once-2" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  const fastInput = (await screen.findByLabelText("fast")) as HTMLInputElement;
  await waitFor(() => expect(fastInput.value).toBe("8"));
  expect(fastInput.placeholder).toBe("3");
  expect(context.navigate).toHaveBeenCalledWith("workspace.lab-strategies", {});
  editScope(() => {
    expect(screen.getByRole("button", { name: "移除 {symbol} SHFE/rb" })).toBeTruthy();
    expect((screen.getByLabelText("初始资金") as HTMLInputElement).value).toBe("500000");
  });
  const processesInput = screen.getByLabelText("并行进程数") as HTMLInputElement;
  expect(processesInput.value).toBe(String(Math.max(1, (navigator.hardwareConcurrency || 2) - 1)));
  expect(processesInput.placeholder).toBe(processesInput.value);

  const recent = await screen.findByRole("region", { name: "最近运行" });
  expect(recent.textContent).not.toContain("other-1");
  fireEvent.click(screen.getByRole("button", { name: /快线/ }));
  expect(await screen.findByRole("heading", { name: "快线" })).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "全部回测记录" }));
  expect(context.navigate).toHaveBeenCalledWith("workspace.lab-runs", {});
});

test("multiple symbols default to separate runs for single-symbol strategies and portfolio for on_bars strategies", async () => {
  const calls: { kind: string; symbols: string[] }[] = [];
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          strategies: [
            { id: "breakout.Breakout", portfolio: false },
            { id: "composite.CompositePortfolio", portfolio: true },
          ],
        }) as never;
      case "listRecentRuns":
        return create(RunsSchema, {}) as never;
      case "readSource":
        return create(SourceSchema, { content: "" }) as never;
      case "startSweep":
        calls.push({ kind: "sweep", symbols: [...operation.value.symbols!] });
        return {} as never;
      case "startBacktest":
        calls.push({ kind: "backtest", symbols: [...operation.value.symbols!] });
        return {} as never;
      case "getSweep":
        return create(SweepRunsSchema, {
          runs: [
            {
              id: "s-0",
              symbols: ["SHFE/rb"],
              firstDay: "2020-01-02",
              lastDay: "2026-02-13",
              state: RunState.SUCCEEDED,
              metrics: { total_return: 0.2, sharpe: 1.5, max_drawdown: 0.1 },
            },
            {
              id: "s-1",
              symbols: ["DCE/i"],
              firstDay: "2021-01-04",
              lastDay: "2026-02-13",
              state: RunState.SUCCEEDED,
              metrics: { total_return: 0.1, sharpe: 1.1, max_drawdown: 0.08 },
            },
          ],
        }) as never;
      case "getRunSummary":
        return create(RunSummarySchema, {
          id: operation.value.id,
          state: RunState.SUCCEEDED,
        }) as never;
      case "getRun":
        return mockRunDetail(operation.value.id!) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "Breakout" }));
  await screen.findByRole("button", { name: "运行" });
  expect(screen.queryByRole("radio", { name: "分别回测" })).toBeNull();

  editScope(() => {
    fireEvent.focus(screen.getByLabelText("品种"));
    fireEvent.click(screen.getByRole("option", { name: /SHFE\/rb/ }));
    expect(screen.queryByRole("radio", { name: "分别回测" })).toBeNull();
    fireEvent.click(screen.getByRole("option", { name: /DCE\/i/ }));
  });
  expect((screen.getByRole("radio", { name: "分别回测" }) as HTMLInputElement).checked).toBe(true);

  fireEvent.click(screen.getByRole("button", { name: "运行" }));
  await waitFor(() => expect(calls).toEqual([{ kind: "sweep", symbols: ["SHFE/rb", "DCE/i"] }]));
  expect(await screen.findByText("2020-01-02 – 2026-02-13")).toBeTruthy();

  fireEvent.change(screen.getByLabelText("切换策略"), {
    target: { value: "composite.CompositePortfolio" },
  });
  expect((screen.getByRole("radio", { name: "组合合跑" }) as HTMLInputElement).checked).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "运行" }));
  await waitFor(() =>
    expect(calls).toEqual([
      { kind: "sweep", symbols: ["SHFE/rb", "DCE/i"] },
      { kind: "backtest", symbols: ["SHFE/rb", "DCE/i"] },
    ]),
  );
});

test("editor indents four spaces on Tab, saves on Ctrl+S, and duplicates or deletes via context menu", async () => {
  const files: Record<string, { content: string; readOnly: boolean }> = {
    "strategies/builtin.py": {
      content: "from strategies._common import helper\nclass Builtin(Strategy):\n    pass\n",
      readOnly: true,
    },
  };
  const listed = [{ id: "builtin.Builtin" }];
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, { strategies: [...listed] }) as never;
      case "listRecentRuns":
        return create(RunsSchema, {}) as never;
      case "readSource": {
        const entry = files[operation.value.path!];
        if (!entry) {
          if (operation.value.path === "strategies/_common.py")
            return create(SourceSchema, { content: "", readOnly: true }) as never;
          throw new Error(`no module ${operation.value.path}`);
        }
        return create(SourceSchema, entry) as never;
      }
      case "saveSource": {
        const savedPath = operation.value.source!.path!;
        const content = operation.value.content!;
        files[savedPath] = { content, readOnly: false };
        const stem = savedPath.replace(/^strategies\//, "").replace(/\.py$/, "");
        const cls = content.match(/class\s+([A-Za-z_][A-Za-z0-9_]*)/)?.[1];
        for (let i = listed.length - 1; i >= 0; i--) {
          if (listed[i]!.id.startsWith(`${stem}.`)) listed.splice(i, 1);
        }
        if (cls) {
          listed.push({ id: `${stem}.${cls}` });
          listed.sort((a, b) => a.id.localeCompare(b.id));
        }
        return {} as never;
      }
      case "deleteSource": {
        delete files[operation.value.path!];
        const stem = operation.value.path!.replace(/^strategies\//, "").replace(/\.py$/, "");
        for (let i = listed.length - 1; i >= 0; i--) {
          if (listed[i]!.id.startsWith(`${stem}.`)) listed.splice(i, 1);
        }
        return {} as never;
      }
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "Builtin" }));
  const imported = await waitFor(() => {
    const element = document.querySelector('[data-module="strategies/_common.py"]');
    expect(element).toBeTruthy();
    return element!;
  });
  expect(screen.getByText("只读")).toBeTruthy();
  expect(screen.queryByRole("button", { name: /^返回/ })).toBeNull();
  fireEvent.mouseDown(imported);
  await waitFor(() =>
    expect(screen.getByRole("region", { name: "strategies/_common.py" })).toBeTruthy(),
  );
  const backButton = screen.getByRole("button", { name: "返回 {path} strategies/builtin.py" });
  expect(backButton).toBeTruthy();
  fireEvent.keyDown(backButton, { key: "ArrowLeft", altKey: true });
  await waitFor(() =>
    expect(screen.getByRole("region", { name: "strategies/builtin.py" })).toBeTruthy(),
  );
  expect(screen.queryByRole("button", { name: /^返回/ })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "← 策略库" }));
  fireEvent.contextMenu(screen.getByRole("button", { name: "Builtin" }));
  await waitFor(() =>
    expect((screen.getByRole("menuitem", { name: "复制" }) as HTMLButtonElement).disabled).toBe(
      false,
    ),
  );
  expect(screen.queryByRole("menuitem", { name: "删除" })).toBeNull();
  fireEvent.click(screen.getByRole("menuitem", { name: "复制" }));
  fireEvent.change(screen.getByLabelText("模块名"), { target: { value: "aaaaa" } });
  fireEvent.click(screen.getByRole("button", { name: "创建" }));
  await waitFor(() =>
    expect(screen.getByRole("region", { name: "strategies/aaaaa.py" })).toBeTruthy(),
  );
  await waitFor(() => expect(editor().state.readOnly).toBe(false));
  expect(editor().state.doc.toString()).toBe(
    "from strategies._common import helper\nclass Aaaaa(Strategy):\n    pass\n",
  );
  editor().dispatch({ selection: { anchor: 0 } });
  fireEvent.keyDown(editor().contentDOM, { key: "Tab" });
  expect(editor().state.doc.toString()).toBe(
    "    from strategies._common import helper\nclass Aaaaa(Strategy):\n    pass\n",
  );
  const renamedDoc = editor().state.doc.toString().replace("class Aaaaa", "class Custom");
  editor().dispatch({
    changes: { from: 0, to: editor().state.doc.length, insert: renamedDoc },
    selection: { anchor: renamedDoc.length },
  });
  fireEvent.keyDown(editor().contentDOM, { key: "Tab", shiftKey: true });
  fireEvent.keyDown(editor().contentDOM, { key: "Tab" });
  fireEvent.keyDown(editor().contentDOM, { key: "s", ctrlKey: true });
  await waitFor(() =>
    expect(files["strategies/aaaaa.py"]?.content).toBe(
      "    from strategies._common import helper\nclass Custom(Strategy):\n    pass\n    ",
    ),
  );
  await waitFor(() =>
    expect((screen.getByLabelText("切换策略") as HTMLSelectElement).value).toBe("aaaaa.Custom"),
  );
  fireEvent.click(screen.getByRole("button", { name: "← 策略库" }));
  fireEvent.contextMenu(await screen.findByRole("button", { name: "Custom" }));
  const del = await screen.findByRole("menuitem", { name: "删除" });
  fireEvent.click(del);
  fireEvent.click(screen.getByRole("menuitem", { name: "确认删除 {name} strategies/aaaaa.py" }));
  await waitFor(() => expect(screen.queryByRole("button", { name: "Custom" })).toBeNull());
  expect(screen.getByRole("button", { name: "Builtin" })).toBeTruthy();
  expect(screen.queryByRole("alert")).toBeNull();
});

test("selected strategies run as one batch and compare on the same settings", async () => {
  const started: { strategies?: { strategy?: string; params?: Record<string, unknown> }[] }[] = [];
  const member = (id: string, strategy: string, state: RunState, sharpe?: number) => ({
    id,
    strategy,
    state,
    symbols: ["SHFE/rb"],
    firstDay: "2026-01-05",
    lastDay: "2026-02-13",
    period: Period.DAY,
    settings: {
      deposit: { text: "1000000" },
      slippageTicks: 1,
      limits: {
        maxOrderLots: { text: "100" },
        maxContractLots: { text: "500" },
        maxGrossLots: { text: "2000" },
        maxWorkingOrders: 64,
      },
      maxGrossWeight: { text: "1" },
      maxNetWeight: { text: "1" },
      periodsPerYear: 252,
      zeroRiskFreeRate: true,
    },
    error: state === RunState.FAILED ? "no data" : "",
    params: { __strategy__: { value: { case: "text" as const, value: strategy } } },
    metrics: (sharpe === undefined ? {} : { sharpe }) as Record<string, number>,
  });
  const finished = () => [
    member("batch-0", "a.Alpha", RunState.SUCCEEDED, 1.5),
    member("batch-1", "b.Beta", RunState.FAILED),
  ];
  vi.mocked(lab).mockReset();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          strategies: [
            {
              id: "a.Alpha",
              parameters: [{ name: "fast", default: { value: { case: "integer", value: 2n } } }],
            },
            { id: "b.Beta", portfolio: true },
          ],
        }) as never;
      case "listRecentRuns":
        return create(RunsSchema, { runs: started.length ? finished() : [] }) as never;
      case "readSource":
        return create(SourceSchema, { content: "x = 1\n" }) as never;
      case "startSweep":
        started.push(operation.value as never);
        return {} as never;
      case "getSweep":
        return create(SweepRunsSchema, {
          active: false,
          runs: finished(),
        }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-strategies">
      <StrategyLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByLabelText("选择 {name} a.Alpha"));
  fireEvent.click(screen.getByLabelText("选择 {name} b.Beta"));
  editScope(() =>
    fireEvent.change(screen.getByPlaceholderText("选择品种或合约…"), {
      target: { value: "SHFE/rb," },
    }),
  );
  fireEvent.click(screen.getByRole("button", { name: "批量回测（{count}） 2" }));
  await waitFor(() => expect(started).toHaveLength(1));
  expect(started[0]!.strategies!.map(item => item.strategy)).toEqual(["a.Alpha", "b.Beta"]);
  expect(started[0]!.strategies![0]!.params!.fast).toEqual({
    value: { case: "integer", value: 2n },
  });
  expect(await screen.findByText("1.500")).toBeTruthy();
  expect(screen.getByText("回测失败")).toBeTruthy();
  expect(screen.getByText("失败")).toBeTruthy();
  expect(screen.queryByText("条件不同")).toBeNull();
});
