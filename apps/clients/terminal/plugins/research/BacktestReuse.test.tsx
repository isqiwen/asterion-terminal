import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { BacktestReuse } from "./BacktestReuse";
import { plugin } from "./plugin";
import { backtestEvidence } from "./testing/backtestEvidence";

registerTerminalPlugins([plugin]);

function setup(evidence = backtestEvidence("original")) {
  const run = vi.fn(async () => true);
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="reuse">
      <BacktestReuse
        evidence={evidence}
        snapshot={readySnapshot()}
        busy={false}
        query={vi.fn()}
        run={run}
      />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(screen.getByLabelText("沿用原固定快照"));
  return run;
}

test("one cost variant reuses the original fixed data with only that cost changed", async () => {
  const evidence = backtestEvidence("original");
  evidence.experiment.series = [];
  const run = setup(evidence);
  fireEvent.change(screen.getByLabelText("成本变体"), { target: { value: "slippage" } });
  const create = screen.getByRole("button", { name: "创建独立回测任务" });
  expect(create).toHaveProperty("disabled", true);
  fireEvent.change(screen.getByLabelText("滑点跳数（0–100 的整数）"), { target: { value: "2" } });
  fireEvent.click(create);
  await vi.waitFor(() => expect(run).toHaveBeenCalledTimes(1));
  expect(run).toHaveBeenCalledWith(
    "backtest.reuse",
    expect.objectContaining({ mode: "fixed", snapshot: "", slippage_ticks: 2 }),
  );
  expect(run.mock.calls[0][1]).not.toHaveProperty("fee_multiplier");
});

test("a backtest Research cannot vary offers no cost variant", () => {
  setup(backtestEvidence("dominant-series"));
  expect(screen.queryByLabelText("成本变体")).toBeNull();
});
