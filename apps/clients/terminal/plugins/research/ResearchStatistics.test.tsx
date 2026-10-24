import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { TaskResult, ResourceObject } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { ResearchStatistics } from "./ResearchStatistics";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
test("saved zero, missing states, original precision and bin membership are displayed without new computation", () => {
  const snapshot = readySnapshot();
  snapshot.research_service!.owner_node_id = "fixture-node";
  const source: ResourceObject = {
    node: "fixture-node",
    service: "research",
    domain: "research",
    kind: "factor_result",
    id: "source",
    version: "a".repeat(64),
  };
  const zero = { state: "DEFINED" as const, value: 0 };
  const result: Extract<TaskResult, { kind: "statistics" }> = {
    id: "report",
    kind: "statistics",
    input_sha256: "b".repeat(64),
    task: {
      id: "report",
      kind: "statistics",
      state: "succeeded",
      attempt: 1,
      completed: 6,
      total: 6,
      error: "",
      result_digest: "c".repeat(64),
      trading_day: "",
      instrument: "source",
      source_name: "research",
      submission_sequence: 2,
      submitted_at_ms: 1,
      updated_at_ms: 2,
      program_artifact: "d".repeat(64),
    },
    experiment: {
      owner: { node_id: "fixture-node", service_id: "research", domain: "research" },
      request: {
        source,
        use_scope: "saved small range",
        column: "feature",
        role: "full_sample",
        begin_index: 0,
        end_index: 3,
        bins: 8,
      },
      source: {
        result: source,
        input_sha256: "e".repeat(64),
        input_bytes: "4000",
        attempt: 1,
        program_artifact: "f".repeat(64),
        observations: 90,
        price_power: 0,
        required_history: 2,
        horizon: 3,
        evaluation: { mode: "full_sample" },
      },
    },
    result: {
      owner: { node_id: "fixture-node", service_id: "research", domain: "research" },
      source,
      price_power: 0,
      summary: {
        total: 3,
        allowed: 3,
        valid: 1,
        missing: 2,
        mean: zero,
        standard_deviation: { state: "INSUFFICIENT_SAMPLES", value: null },
        minimum: zero,
        q25: zero,
        median: zero,
        q75: zero,
        maximum: zero,
      },
      groups: [],
      reasons: [{ reason: "WARMUP", count: 2 }],
      bins: [{ lower: 0, upper: 0, count: 1 }],
      rows: [
        {
          event_index: 0,
          trading_day: "",
          observed: "2026-09-25T01:00:00.000000123Z",
          value: null,
          reason: "WARMUP",
          bin: null,
        },
        {
          event_index: 1,
          trading_day: "",
          observed: "2026-09-25T01:00:01.000000123Z",
          value: null,
          reason: "WARMUP",
          bin: null,
        },
        {
          event_index: 2,
          trading_day: "",
          observed: "2026-09-25T01:00:02.000000123Z",
          value: 0,
          reason: "VALID",
          bin: 0,
        },
      ],
    },
  };
  snapshot.task_result = result;
  const trade = vi.fn<TerminalContext["trade"]>(async () => {});
  const query = vi.fn<TerminalContext["query"]>(async () => snapshot);
  const context: TerminalContext = {
    snapshot,
    trade,
    query,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
  };
  const drafts = createWorkspaceDrafts();
  const mounted = render(
    <WorkspaceDraftScope store={drafts} namespace="statistics">
      <ResearchStatistics {...context} />
    </WorkspaceDraftScope>,
  );
  const report = screen.getByRole("region", { name: "正式统计报告" });
  expect(
    within(within(report).getByText("均值", { exact: true }).parentElement!).getByText("0", {
      exact: true,
    }).textContent,
  ).toBe("0");
  expect(
    within(within(report).getByText("样本标准差", { exact: true }).parentElement!).getByText(
      "样本不足",
      { exact: true },
    ).textContent,
  ).toBe("样本不足");
  const rows = () =>
    within(screen.getByRole("table", { name: "统计逐行证据" })).getAllByRole("row");
  expect(rows()).toHaveLength(4);
  expect(within(rows()[1]).getAllByRole("cell")[2].textContent).toBe("—");
  expect(within(rows()[3]).getAllByRole("cell")[1].textContent).toBe(
    result.result.rows[2].observed,
  );
  expect(within(rows()[3]).getAllByRole("cell")[2].textContent).toBe("0");
  fireEvent.click(
    within(screen.getByRole("region", { name: "原报告直方图" })).getAllByRole("button")[1],
  );
  expect(rows()).toHaveLength(2);
  expect(within(rows()[1]).getAllByRole("cell")[0].textContent).toBe("2");
  expect(trade).not.toHaveBeenCalled();
  expect(query).not.toHaveBeenCalled();
  const grouped = structuredClone(result);
  const defined = (value: number) => ({ state: "DEFINED" as const, value });
  grouped.experiment.request.source.kind = "sample_result";
  grouped.experiment.request.column = "raw_close";
  grouped.experiment.request.grouping = "trading_day";
  grouped.experiment.request.bins = 2;
  grouped.experiment.source.result.kind = "sample_result";
  grouped.experiment.source.required_history = 0;
  grouped.experiment.source.horizon = 0;
  grouped.experiment.source.observations = 3;
  grouped.experiment.source.price_power = 1;
  grouped.result.source.kind = "sample_result";
  grouped.result.price_power = 1;
  grouped.result.rows = [100, -1, 0].map((value, index) => ({
    event_index: index,
    value,
    reason: "VALID" as const,
    bin: index === 0 ? 1 : 0,
    trading_day: index < 2 ? "2026-09-25" : "2026-09-28",
    observed:
      index === 0
        ? "2026-09-24T13:00:00.000000123Z"
        : index === 1
          ? "2026-09-25T01:00:00.000000123Z"
          : "2026-09-28T01:00:00.000000123Z",
  }));
  grouped.result.summary = {
    total: 3,
    allowed: 3,
    valid: 3,
    missing: 0,
    mean: defined(33),
    standard_deviation: defined(Math.sqrt(3367)),
    minimum: defined(-1),
    q25: defined(-0.5),
    median: defined(0),
    q75: defined(50),
    maximum: defined(100),
  };
  grouped.result.bins = [
    { lower: -1, upper: 49.5, count: 2 },
    { lower: 49.5, upper: 100, count: 1 },
  ];
  grouped.result.reasons = [{ reason: "VALID", count: 3 }];
  grouped.result.groups = [
    {
      trading_day: "2026-09-25",
      summary: {
        ...grouped.result.summary,
        total: 2,
        allowed: 2,
        valid: 2,
        mean: defined(49.5),
        standard_deviation: defined(101 / Math.sqrt(2)),
        q25: defined(24.25),
        median: defined(49.5),
        q75: defined(74.75),
      },
      reasons: [{ reason: "VALID", count: 2 }],
      bin_counts: [1, 1],
    },
    {
      trading_day: "2026-09-28",
      summary: {
        ...grouped.result.summary,
        total: 1,
        allowed: 1,
        valid: 1,
        mean: defined(0),
        standard_deviation: { state: "INSUFFICIENT_SAMPLES", value: null },
        minimum: defined(0),
        q25: defined(0),
        median: defined(0),
        q75: defined(0),
        maximum: defined(0),
      },
      reasons: [{ reason: "VALID", count: 1 }],
      bin_counts: [1, 0],
    },
  ];
  snapshot.task_result = grouped;
  mounted.rerender(
    <WorkspaceDraftScope store={drafts} namespace="statistics">
      <ResearchStatistics {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.change(screen.getByLabelText("查看已保存交易日分组"), {
    target: { value: "2026-09-25" },
  });
  expect(rows()).toHaveLength(3); // Selecting a group clears the previous bin filter.
  expect(within(rows()[1]).getAllByRole("cell")[1].textContent).toBe(
    "2026-09-24T13:00:00.000000123Z",
  );
  expect(within(rows()[1]).getAllByRole("cell")[5].textContent).toBe("2026-09-25");
  expect(
    within(within(report).getByText("均值", { exact: true }).parentElement!).getByText("49.5", {
      exact: true,
    }).textContent,
  ).toBe("49.5");
  expect(trade).not.toHaveBeenCalled();
  expect(query).not.toHaveBeenCalled();
});
