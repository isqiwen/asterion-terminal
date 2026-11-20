import { create } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { tradingRequest } from "../../src/bridge/trading";
import { RunState, RunSummarySchema } from "../../src/generated/asterion/lab_pb";
import {
  Environment,
  InstanceState,
  ResponseSchema,
} from "../../src/generated/asterion/trading_pb";
import { TradingAccountsProvider } from "../../src/host/trading-accounts";
import { StrategyLifecycle } from "./StrategyLifecycle";

vi.mock("../../src/bridge/trading", () => ({ tradingRequest: vi.fn() }));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string) => key,
}));

const run = (id: string, startedMs: bigint, state = RunState.SUCCEEDED) =>
  create(RunSummarySchema, {
    id,
    strategy: "ma_cross.MaCross",
    state,
    startedMs,
    metrics: { total_return: 0.0015 },
  });

test("each step names the next one until the strategy runs in simulation", async () => {
  vi.useFakeTimers();
  let published = false;
  vi.mocked(tradingRequest).mockImplementation(async (id, operation) =>
    operation.case === "getAccount"
      ? create(ResponseSchema, {
          result: { case: "getAccount", value: { id, environment: Environment.SIMULATION } },
        })
      : create(ResponseSchema, {
          result: {
            case: "getStrategies",
            value: published
              ? {
                  packages: [
                    { name: "ma_cross.MaCross", version: "20261118-090000" },
                    {
                      name: "ma_cross.MaCross",
                      version: "20261119-183952",
                      research: { runId: "latest" },
                    },
                  ],
                  instance: {
                    state: InstanceState.READY,
                    instance: { id: "soak", packageName: "ma_cross.MaCross" },
                  },
                }
              : {},
          },
        }),
  );
  const deploy = vi.fn();
  const openTrading = vi.fn();
  let runs = [run("old", 1n), run("latest", 2n), run("failed", 3n, RunState.FAILED)];
  const view = () => (
    <TradingAccountsProvider
      connections={[{ id: "trading-uuid-sim", revision: "1", instance: "simnow" }]}
    >
      <StrategyLifecycle
        strategy="ma_cross.MaCross"
        path="strategies/ma_cross.py"
        unsaved={false}
        runs={runs}
        openCode={() => {}}
        backtest={() => {}}
        openRun={() => {}}
        deploy={deploy}
        openTrading={openTrading}
      />
    </TradingAccountsProvider>
  );
  const page = render(view());
  await act(() => vi.advanceTimersByTimeAsync(0));
  const steps = screen.getByRole("list", { name: "上线进度" });
  expect(steps.textContent).toContain("部署后仿真");
  fireEvent.click(screen.getByRole("button", { name: "去部署" }));
  expect(deploy).toHaveBeenCalledWith("latest");

  published = true;
  page.rerender(view());
  await act(() => vi.advanceTimersByTimeAsync(6000));
  fireEvent.click(screen.getByRole("button", { name: "20261119-183952 · simnow" }));
  expect(openTrading).toHaveBeenLastCalledWith("trading-uuid-sim");
  // A simulation account runs it, so going live is the next step.
  const [, , , simulation, live] = within(steps).getAllByRole("listitem");
  expect(simulation!.textContent).toBe("仿真simnow");
  expect(live!.textContent).toBe("实盘去启动");
  expect(screen.queryByRole("button", { name: "去部署" })).toBeNull();
  expect(screen.queryByRole("button", { name: "部署新版" })).toBeNull();

  runs = [...runs, run("newer", 4n)];
  page.rerender(view());
  fireEvent.click(screen.getByRole("button", { name: "部署新版" }));
  expect(deploy).toHaveBeenLastCalledWith("newer");
});
