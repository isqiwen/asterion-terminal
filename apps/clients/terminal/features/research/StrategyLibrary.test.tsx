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

const library = (props: Partial<Parameters<typeof StrategyLibrary>[0]>) =>
  render(
    <StrategyLibrary
      items={[]}
      runs={[]}
      views={{}}
      chosen=""
      selected={[]}
      setSelected={vi.fn()}
      batch={null}
      search=""
      setSearch={vi.fn()}
      busy={false}
      startBatch={vi.fn()}
      cancelBatch={vi.fn()}
      open={vi.fn()}
      onMenu={vi.fn()}
      newModule={null}
      notices={null}
      {...props}
    />,
  );
const order = () =>
  within(screen.getByRole("table", { name: "策略库" }))
    .getAllByRole("rowheader")
    .map(header => header.textContent);

test("strategies rank by their latest success and show how far they reached", () => {
  const items = [
    item("Alpha", "单品种"),
    item("Beta", "组合"),
    item("Gamma", "组合"),
    item("Delta", "单品种"),
  ];
  const runs = [
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
  library({ items, runs, views, open });
  const table = screen.getByRole("table", { name: "策略库" });
  expect(order()).toEqual(["Beta", "Alpha", "Delta", "Gamma"]);
  const delta = within(table).getByRole("row", { name: /m\.Delta/ });
  expect(within(delta).getByText("无成交")).toBeTruthy();
  const beta = within(table).getByRole("row", { name: /m\.Beta/ });
  expect(within(beta).getByText("仿真")).toBeTruthy();
  const alpha = within(table).getByRole("row", { name: /m\.Alpha/ });
  expect(within(alpha).getByText("已回测")).toBeTruthy();
  expect(within(alpha).getByText("1.000")).toBeTruthy();

  fireEvent.click(alpha);
  expect(open).toHaveBeenCalledWith(items[0], "alpha");
  fireEvent.click(within(table).getByRole("button", { name: "Gamma" }));
  expect(open).toHaveBeenCalledWith(items[2], undefined);

  fireEvent.click(screen.getByRole("radio", { name: "组合" }));
  expect(order()).toEqual(["Beta", "Gamma"]);
});

test("a strategy whose latest outcome failed leaves the ranking and opens on the failure", () => {
  const items = [item("Alpha", "单品种"), item("Beta", "单品种")];
  const runs = [
    run("alpha-failed", "Alpha", RunState.FAILED, 0),
    run("beta", "Beta", RunState.SUCCEEDED, 1),
    run("alpha", "Alpha", RunState.SUCCEEDED, 9),
  ];
  const open = vi.fn();
  library({ items, runs, open });
  expect(order()).toEqual(["Beta", "Alpha"]);
  const table = screen.getByRole("table", { name: "策略库" });
  expect(within(table).getByRole("columnheader", { name: "回测失败" })).toBeTruthy();
  const alpha = within(table).getByRole("row", { name: /m\.Alpha/ });
  expect(within(alpha).getByText("失败")).toBeTruthy();
  expect(within(alpha).queryByText("9.000")).toBeNull();
  fireEvent.click(alpha);
  expect(open).toHaveBeenCalledWith(items[0], "alpha-failed");
});

test("a cancelled run or a failure on other settings keeps the result", () => {
  const items = [item("Alpha", "单品种")];
  const runs = [
    run("alpha-stopped", "Alpha", RunState.STOPPED, 0),
    run("alpha-other", "Alpha", RunState.FAILED, 0),
    run("alpha", "Alpha", RunState.SUCCEEDED, 1),
  ];
  library({ items, runs, onScope: entry => entry.id !== "alpha-other" });
  const table = screen.getByRole("table", { name: "策略库" });
  expect(within(table).getByText("1.000")).toBeTruthy();
  expect(within(table).queryByRole("columnheader", { name: "回测失败" })).toBeNull();
});

test("only results on the current settings rank", () => {
  const items = [item("Alpha", "单品种"), item("Beta", "单品种"), item("Gamma", "单品种")];
  // Beta's better figure was measured on other settings.
  const runs = [
    run("beta-other", "Beta", RunState.SUCCEEDED, 9),
    run("alpha-same", "Alpha", RunState.SUCCEEDED, 1),
    run("alpha-other", "Alpha", RunState.SUCCEEDED, 5),
  ];
  library({ items, runs, onScope: entry => entry.id === "alpha-same" });
  const table = screen.getByRole("table", { name: "策略库" });
  expect(order()).toEqual(["Alpha", "Beta", "Gamma"]);
  expect(
    within(within(table).getByRole("row", { name: /m\.Alpha/ })).getByText("1.000"),
  ).toBeTruthy();
  const titles = within(table)
    .getAllByRole("columnheader")
    .map(header => header.textContent);
  expect(titles).toEqual(expect.arrayContaining(["条件不同", "未回测"]));
});
