import { create } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { tradingRequest } from "../bridge/trading";
import {
  HealthSchema,
  InstanceState,
  ResponseSchema,
  StrategiesSchema,
  type Response,
} from "../generated/asterion/trading_pb";
import { notify } from "./notifications";
import { TradingAccountsProvider, useTradingAccounts } from "./trading-accounts";

vi.mock("../bridge/trading", () => ({ tradingRequest: vi.fn() }));
vi.mock("./notifications", () => ({ notify: vi.fn() }));

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
      return create(ResponseSchema, { result: { case: "getHealth", value: { protocol: "ours" } } });
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
    <TradingAccountsProvider ids={["sim"]} protocol="ours">
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

test("a Trading service built from another protocol is reported and stays usable", async () => {
  vi.mocked(tradingRequest).mockResolvedValue(
    create(ResponseSchema, {
      result: { case: "getHealth", value: create(HealthSchema, { protocol: "theirs" }) },
    }),
  );
  render(
    <TradingAccountsProvider ids={["sim"]} protocol="ours">
      <span />
    </TradingAccountsProvider>,
  );
  await waitFor(() => expect(notify).toHaveBeenCalledOnce());
  expect(vi.mocked(notify).mock.calls[0]![0]).toMatchObject({ code: "trading_protocol" });
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
          events: events.map(([timeMs, text]) => ({ timeMs, text })),
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
          result: { case: "getHealth", value: create(HealthSchema, { protocol: "ours" }) },
        }),
  );
  const view = (ids: string[]) => (
    <TradingAccountsProvider ids={ids} protocol="ours">
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
