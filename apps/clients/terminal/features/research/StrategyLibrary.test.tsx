import { create } from "@bufbuild/protobuf";
import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { RunState, RunSummarySchema } from "../../src/generated/asterion/lab_pb";
import { Environment, InstanceState } from "../../src/generated/asterion/trading_pb";
import type { TradingView } from "../../src/host/trading-accounts";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";
import type { ModuleItem } from "./LabParts";
import { StrategyLibrary } from "./StrategyLibrary";

registerTerminalFeatures([feature]);

const item = (name: string, group: string): ModuleItem => ({
  id: `m.${name}`,
  name,
  module: "m",
  group,
  description: "",
});
const run = (id: string, name: string, state: RunState, sharpe: number, fills = 3) =>
  create(RunSummarySchema, {
    id,
    strategy: `m.${name}`,
    state,
    symbols: ["SHFE/rb"],
    firstDay: "2026-01-05",
    lastDay: "2026-02-13",
    metrics: { sharpe, total_return: sharpe / 10, fills },
  });

test("strategies rank by their latest success and show how far they reached", () => {
  const items = [
    item("Alpha", "单品种"),
    item("Beta", "组合"),
    item("Gamma", "组合"),
    item("Delta", "单品种"),
  ];
  const runs = [
    run("alpha-failed", "Alpha", RunState.FAILED, 9),
    run("beta", "Beta", RunState.SUCCEEDED, 2),
    run("alpha", "Alpha", RunState.SUCCEEDED, 1),
    run("delta", "Delta", RunState.SUCCEEDED, 0, 0),
  ];
  const views = {
    sim: {
      account: { environment: Environment.SIMULATION },
      strategies: {
        packages: [],
        instance: { state: InstanceState.READY, instance: { packageName: "m.Beta" } },
      },
    },
  } as unknown as Record<string, TradingView>;
  const open = vi.fn();
  const openRun = vi.fn();
  render(
    <StrategyLibrary
      items={items}
      total={4}
      runs={runs}
      views={views}
      chosen=""
      selected={[]}
      setSelected={vi.fn()}
      batch={null}
      scope="all"
      setScope={vi.fn()}
      cancelBatch={vi.fn()}
      open={open}
      openRun={openRun}
    />,
  );
  const table = screen.getByRole("table", { name: "策略库" });
  const order = () =>
    within(table)
      .getAllByRole("rowheader")
      .map(header => header.textContent);
  expect(order()).toEqual(["Beta", "Alpha", "Delta", "Gamma"]);
  const delta = within(table).getByRole("row", { name: /m\.Delta/ });
  expect(within(delta).getByText("无成交")).toBeTruthy();
  const beta = within(table).getByRole("row", { name: /m\.Beta/ });
  expect(within(beta).getByText("仿真")).toBeTruthy();
  const alpha = within(table).getByRole("row", { name: /m\.Alpha/ });
  expect(within(alpha).getByText("已回测")).toBeTruthy();
  expect(within(alpha).getByText("1.000")).toBeTruthy();

  fireEvent.click(within(alpha).getByRole("button", { name: "Alpha 总收益" }));
  expect(openRun).toHaveBeenCalledWith(items[0], "alpha");
  expect(open).not.toHaveBeenCalled();
  fireEvent.click(within(table).getByRole("button", { name: "Gamma" }));
  expect(open).toHaveBeenCalledWith(items[2]);

  fireEvent.change(screen.getByLabelText("类型"), { target: { value: "组合" } });
  expect(order()).toEqual(["Beta", "Gamma"]);
});
