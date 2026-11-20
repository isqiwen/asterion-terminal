import { create } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { StrategyMonitor } from "./StrategyMonitor";
import { feature } from "./feature";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import {
  BrokerState,
  InstanceState,
  InstanceStatusSchema,
  ResponseSchema,
} from "../../src/generated/asterion/trading_pb";
import { Side } from "../../src/generated/asterion/common_pb";
import { tradingRequest } from "../../src/bridge/trading";
import { TradingAccountsProvider, useTradingAccounts } from "../../src/host/trading-accounts";

vi.mock("../../src/bridge/trading", () => ({ tradingRequest: vi.fn() }));

registerTerminalFeatures([feature]);
test("positions show their account observation and retain it through a failed read", async () => {
  vi.useFakeTimers();
  const instrument = { exchange: "SHFE", symbol: "rb2610" };
  const snapshot = (positionsReconciled: boolean, lots?: string) =>
    create(ResponseSchema, {
      result: {
        case: "getAccount",
        value: {
          session: {
            brokerState: BrokerState.READY,
            positionsReconciled,
            positions: lots ? [{ instrument, side: Side.BUY, todayLots: { text: lots } }] : [],
          },
        },
      },
    });
  let account = snapshot(true, "3");
  let online = false;
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) => {
    if (operation.case === "getAccount") {
      if (!online) throw new Error("offline");
      return account;
    }
    return create(ResponseSchema, { result: { case: "getStrategies", value: {} } });
  });
  const instance = create(InstanceStatusSchema, {
    instance: { id: "run", instruments: [instrument] },
  });
  function Monitor() {
    return (
      <StrategyMonitor
        instance={instance}
        account={useTradingAccounts().views.sim}
        fresh
        stopping={false}
        busy={false}
        stop={vi.fn()}
        pause={vi.fn()}
        flatten={vi.fn()}
      />
    );
  }
  render(
    <TradingAccountsProvider connections={[{ id: "sim", revision: "1" }]}>
      <Monitor />
    </TradingAccountsProvider>,
  );
  const row = within(screen.getByRole("row", { name: /SHFE.rb2610/ }));
  const value = () => row.getAllByRole("cell")[2]!.textContent;
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(value()).toBe("—");
  expect(screen.getByText(/持仓未读取.*服务失联/)).toBeTruthy();
  online = true;
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(value()).toBe("3");
  const readAt = screen.getByText("持仓读取时间").nextElementSibling!.textContent!;
  expect(readAt).toBe(
    new Date().toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false }),
  );
  online = false;
  await act(() => vi.advanceTimersByTimeAsync(2000));
  expect(value()).toBe("3");
  expect(screen.getByText(`${readAt} · 服务失联 · 上次已核对持仓`)).toBeTruthy();
  online = true;
  account = snapshot(false);
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(value()).toBe("3");
  expect(screen.getByText(`${readAt} · 持仓待核对 · 上次已核对持仓`)).toBeTruthy();
  account = snapshot(true);
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(value()).toBe("0");
  vi.useRealTimers();
});

test("stop and cancel remains confirmable while a different strategy command is in flight", async () => {
  const stop = vi.fn(async () => true);
  render(
    <StrategyMonitor
      instance={create(InstanceStatusSchema, {
        instance: { id: "running-strategy" },
        state: InstanceState.READY,
      })}
      fresh={true}
      busy={true}
      stopping={false}
      stop={stop}
      pause={vi.fn()}
      flatten={vi.fn()}
    />,
  );
  fireEvent.click(screen.getByRole("button", { name: "停止策略并撤单" }));
  fireEvent.click(screen.getByRole("button", { name: "确认停止并撤单" }));
  expect(stop).toHaveBeenCalledWith("running-strategy");
  await waitFor(() => expect(screen.queryByRole("button", { name: "确认停止并撤单" })).toBeNull());
});
