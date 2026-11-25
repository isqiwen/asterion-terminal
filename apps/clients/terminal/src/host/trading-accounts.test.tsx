import { create } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { tradingRequest } from "../bridge/trading";
import {
  BrokerState,
  HealthSchema,
  InstanceEventKind,
  InstanceState,
  ResponseSchema,
  StrategiesSchema,
  type Response,
} from "../generated/asterion/trading_pb";
import { BackendError } from "../i18n/errors";
import { notify } from "./notifications";
import { OpenLiveTrading, TradingAccountsProvider, useTradingAccounts } from "./trading-accounts";

vi.mock("../bridge/trading", () => ({ tradingRequest: vi.fn() }));
vi.mock("./notifications", () => ({ notify: vi.fn() }));

test("changing a connection hides its old account while a disconnect keeps the same account", async () => {
  vi.useFakeTimers();
  let account = "first";
  let fail = false;
  let waiting = false;
  const replies: ((response: Response) => void)[] = [];
  const response = (id: string) =>
    create(ResponseSchema, { result: { case: "getAccount", value: { id } } });
  vi.mocked(tradingRequest).mockImplementation(async (_id, operation) => {
    if (operation.case !== "getAccount")
      return create(ResponseSchema, { result: { case: "getStrategies", value: {} } });
    if (fail) throw new Error("offline");
    if (waiting) return new Promise(resolve => replies.push(resolve));
    return response(account);
  });
  function Status() {
    const view = useTradingAccounts().views.sim;
    return (
      <output>
        {view?.account?.id ?? "reading"}
        {view?.failed ? " offline" : ""}
      </output>
    );
  }
  const content = (revision: string) => (
    <TradingAccountsProvider connections={[{ id: "sim", revision }]}>
      <Status />
    </TradingAccountsProvider>
  );
  const page = render(content("1"));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole("status").textContent).toBe("first");
  waiting = true;
  await act(() => vi.advanceTimersByTimeAsync(1000));
  page.rerender(content("2"));
  expect(screen.getByRole("status").textContent).toBe("reading");
  await act(async () => replies[0](response("first")));
  expect(screen.getByRole("status").textContent).toBe("reading");
  account = "second";
  await act(async () => replies[1](response(account)));
  expect(screen.getByRole("status").textContent).toBe("second");
  waiting = false;
  fail = true;
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(screen.getByRole("status").textContent).toBe("second offline");
});

test("a delayed command reply cannot undo the account's observed revocation", async () => {
  vi.useFakeTimers();
  let authorized = true;
  const replies = new Map<string, (response: Response) => void>();
  const account = (operation: "getAccount" | "sendCommand", allowed: boolean) =>
    create(ResponseSchema, {
      result: {
        case: operation,
        value: { id: "record", session: allowed ? { authorizedMs: 1n } : {} },
      },
    });
  vi.mocked(tradingRequest).mockImplementation(async (_id, operation) => {
    if (operation.case === "sendCommand")
      return new Promise(resolve => replies.set(operation.value.requestId!, resolve));
    if (operation.case === "getAccount") return account("getAccount", authorized);
    if (operation.case === "getHealth")
      return create(ResponseSchema, { result: { case: "getHealth", value: {} } });
    return create(ResponseSchema, { result: { case: "getStrategies", value: {} } });
  });
  function Status() {
    const { views, send } = useTradingAccounts();
    const command = (requestId: string) =>
      void send("sim", {
        case: "sendCommand",
        value: {
          requestId,
          accountId: "record",
          policyRevision: "policy",
          action:
            requestId === "cancel"
              ? { case: "cancelOrder", value: { id: "order" } }
              : { case: "revoke", value: {} },
        },
      });
    return (
      <>
        <output>
          {views.sim?.live?.authorizedMs === undefined ? "unauthorized" : "authorized"}
        </output>
        <button onClick={() => command("cancel")}>cancel</button>
        <button onClick={() => command("revoke")}>revoke</button>
      </>
    );
  }
  render(
    <TradingAccountsProvider connections={[{ id: "sim", revision: "1" }]}>
      <Status />
    </TradingAccountsProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole("status").textContent).toBe("authorized");
  fireEvent.click(screen.getByRole("button", { name: "cancel" }));
  fireEvent.click(screen.getByRole("button", { name: "revoke" }));
  await act(async () => {
    authorized = false;
    replies.get("revoke")!(account("sendCommand", false));
    await vi.advanceTimersByTimeAsync(1000);
  });
  expect(screen.getByRole("status").textContent).toBe("unauthorized");
  await act(async () => replies.get("cancel")!(account("sendCommand", true)));
  expect(screen.getByRole("status").textContent).toBe("unauthorized");
  authorized = true;
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(screen.getByRole("status").textContent).toBe("authorized");
});

const instance = (id: string, state: InstanceState, events: [bigint, string][] = [], error = "") =>
  create(ResponseSchema, {
    result: {
      case: "getStrategies",
      value: create(StrategiesSchema, {
        instance: {
          instance: { id },
          state,
          error,
          events: events.map(([timeMs, text]) => ({
            timeMs,
            text,
            kind: InstanceEventKind.BATCH_ABANDONED,
          })),
        },
      }),
    },
  });

test("instance alerts come from the account layer once each, whichever page is open", async () => {
  vi.useFakeTimers();
  vi.mocked(notify).mockReset();
  const shown = vi.fn();
  vi.stubGlobal(
    "Notification",
    class {
      constructor(title: string, options: { body: string }) {
        shown(title, options.body);
      }
    },
  );
  // Before Terminal starts: an abandoned batch, and the instance has failed.
  let strategies = instance(
    "run-a",
    InstanceState.FAILED,
    [[1n, "batch abandoned: old"]],
    "broker rejected",
  );
  vi.mocked(tradingRequest).mockImplementation(async (account, operation) =>
    operation.case === "getStrategies"
      ? account === "sim"
        ? strategies
        : create(ResponseSchema, { result: { case: "getStrategies", value: {} } })
      : create(ResponseSchema, {
          result: { case: "getHealth", value: create(HealthSchema, {}) },
        }),
  );
  const view = (ids: string[]) => (
    <TradingAccountsProvider connections={ids.map(id => ({ id, revision: "1" }))}>
      <span />
    </TradingAccountsProvider>
  );
  const page = render(view(["sim"]));
  await vi.advanceTimersByTimeAsync(0);
  expect(shown.mock.calls).toEqual([["策略实例 run-a", "broker rejected"]]);

  // Other accounts come and go: nothing is told again.
  page.rerender(view(["sim", "other"]));
  page.rerender(view(["sim"]));
  await vi.advanceTimersByTimeAsync(10000);
  expect(shown).toHaveBeenCalledOnce();

  // A new instance drops a batch and later fails, with no trading page open.
  strategies = instance("run-b", InstanceState.READY, [[5n, "batch abandoned: no quote"]]);
  await vi.advanceTimersByTimeAsync(5000);
  strategies = instance(
    "run-b",
    InstanceState.FAILED,
    [[5n, "batch abandoned: no quote"]],
    "strategy crashed",
  );
  await vi.advanceTimersByTimeAsync(10000);
  expect(shown.mock.calls.slice(1)).toEqual([
    ["策略实例 run-b", "batch abandoned: no quote"],
    ["策略实例 run-b", "strategy crashed"],
  ]);
  expect(vi.mocked(notify).mock.calls.map(([error]) => (error as { code: string }).code)).toEqual([
    "strategy_instance",
    "strategy_instance",
    "strategy_instance",
  ]);
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

test("starting the first instance on an initially empty account alerts its abandoned batch", async () => {
  vi.useFakeTimers();
  vi.mocked(notify).mockReset();
  const shown = vi.fn();
  vi.stubGlobal(
    "Notification",
    class {
      constructor(title: string, options: { body: string }) {
        shown(title, options.body);
      }
    },
  );
  let strategies: Response = create(ResponseSchema, {
    result: { case: "getStrategies", value: {} },
  });
  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) =>
    operation.case === "getStrategies"
      ? strategies
      : create(ResponseSchema, {
          result: { case: "getHealth", value: create(HealthSchema, {}) },
        }),
  );
  render(
    <TradingAccountsProvider connections={[{ id: "sim", revision: "1" }]}>
      <span />
    </TradingAccountsProvider>,
  );
  await vi.advanceTimersByTimeAsync(0);
  expect(shown).not.toHaveBeenCalled();

  strategies = instance("first-run", InstanceState.READY, [[10n, "batch abandoned: first"]]);
  await vi.advanceTimersByTimeAsync(5000);
  expect(shown.mock.calls).toEqual([["策略实例 first-run", "batch abandoned: first"]]);
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

test("trading opens first only when, once all accounts are read, one may send orders", async () => {
  vi.useFakeTimers();
  vi.mocked(tradingRequest).mockImplementation(async (id, operation) =>
    operation.case === "getAccount"
      ? create(ResponseSchema, {
          result: {
            case: "getAccount",
            value: { id, session: id === "live" ? { authorizedMs: 1n } : {} },
          },
        })
      : create(ResponseSchema, { result: { case: "getStrategies", value: {} } }),
  );
  const opened = vi.fn();
  const shell = (connections: string[]) => (
    <TradingAccountsProvider connections={connections.map(id => ({ id, revision: "1" }))}>
      <OpenLiveTrading accounts={connections.length} open={opened} />
    </TradingAccountsProvider>
  );
  const idle = render(shell(["sim"]));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(opened).not.toHaveBeenCalled();
  idle.unmount();
  render(shell(["sim", "live"]));
  await act(() => vi.advanceTimersByTimeAsync(0));
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(opened).toHaveBeenCalledTimes(1);
});

test("OpenLiveTrading opens immediately on a live account, ignores slow offline accounts, and honors cancellation", async () => {
  vi.useFakeTimers();
  let resolveSlow: ((response: Response) => void) | undefined;
  vi.mocked(tradingRequest).mockImplementation(async (id, operation) => {
    if (id === "offline" && operation.case === "getAccount")
      return new Promise(resolve => {
        resolveSlow = resolve;
      });
    return operation.case === "getAccount"
      ? create(ResponseSchema, {
          result: {
            case: "getAccount",
            value: { id, session: id === "live" ? { authorizedMs: 1n } : {} },
          },
        })
      : create(ResponseSchema, { result: { case: "getStrategies", value: {} } });
  });

  // One live account opens right away even while another account hangs offline.
  const openedImmediate = vi.fn();
  const immediate = render(
    <TradingAccountsProvider
      connections={[
        { id: "offline", revision: "1" },
        { id: "live", revision: "1" },
      ]}
    >
      <OpenLiveTrading accounts={2} open={openedImmediate} />
    </TradingAccountsProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(openedImmediate).toHaveBeenCalledTimes(1);
  immediate.unmount();

  // After the startup grace window, a late reply from an offline account must not hijack navigation.
  const openedLate = vi.fn();
  const late = render(
    <TradingAccountsProvider
      connections={[
        { id: "sim", revision: "1" },
        { id: "offline", revision: "1" },
      ]}
    >
      <OpenLiveTrading accounts={2} open={openedLate} />
    </TradingAccountsProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(3100));
  await act(async () => {
    resolveSlow?.(
      create(ResponseSchema, {
        result: { case: "getAccount", value: { id: "offline", session: { authorizedMs: 1n } } },
      }),
    );
  });
  expect(openedLate).not.toHaveBeenCalled();
  late.unmount();

  // User navigation cancels auto-open.
  const openedCancelled = vi.fn();
  render(
    <TradingAccountsProvider connections={[{ id: "live", revision: "1" }]}>
      <OpenLiveTrading accounts={1} cancelled open={openedCancelled} />
    </TradingAccountsProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(openedCancelled).not.toHaveBeenCalled();
  vi.useRealTimers();
});

test("broker login failures notify once per failure and re-notify after explicit reconnect", async () => {
  vi.useFakeTimers();
  vi.mocked(notify).mockReset();
  let session = { brokerState: BrokerState.FAILED, ctpError: 3 };
  vi.mocked(tradingRequest).mockImplementation(async (_id, operation) => {
    if (operation.case === "getAccount" || operation.case === "connectBroker")
      return create(ResponseSchema, {
        result: { case: operation.case, value: { id: "rec", session } },
      });
    if (operation.case === "getHealth")
      return create(ResponseSchema, { result: { case: "getHealth", value: {} } });
    return create(ResponseSchema, { result: { case: "getStrategies", value: {} } });
  });
  function ReconnectButton() {
    const { send } = useTradingAccounts();
    return (
      <button onClick={() => void send("sim", { case: "connectBroker", value: {} })}>
        reconnect
      </button>
    );
  }
  render(
    <TradingAccountsProvider connections={[{ id: "sim", revision: "1", instance: "simnow" }]}>
      <ReconnectButton />
    </TradingAccountsProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(0));
  await act(() => vi.advanceTimersByTimeAsync(2000));
  expect(vi.mocked(notify)).toHaveBeenCalledTimes(1);
  expect(vi.mocked(notify).mock.calls[0]?.[0]).toEqual(
    new BackendError("unavailable", "CTP trading rejected: simnow: 3"),
  );

  // Explicit reconnect clears dedupe so another immediate refusal notifies again.
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name: "reconnect" }));
    await vi.advanceTimersByTimeAsync(1000);
  });
  expect(vi.mocked(notify)).toHaveBeenCalledTimes(2);

  // Recovering clears failure tracking so a later failure notifies again.
  session = { brokerState: BrokerState.READY, ctpError: 0 };
  await act(() => vi.advanceTimersByTimeAsync(1000));
  session = { brokerState: BrokerState.FAILED, ctpError: 63 };
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(vi.mocked(notify)).toHaveBeenCalledTimes(3);
  expect(vi.mocked(notify).mock.calls[2]?.[0]).toEqual(
    new BackendError("unavailable", "CTP trading rejected: simnow: 63"),
  );
  vi.useRealTimers();
});
