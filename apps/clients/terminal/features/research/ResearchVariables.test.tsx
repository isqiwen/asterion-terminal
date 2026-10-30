import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { ResearchVariableDefinition } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { ResearchVariables } from "./ResearchVariables";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
test("an uncertain registration retries the same declaration and operation before freezing the confirmed original", async () => {
  const snapshot = readySnapshot();
  const store = createWorkspaceDrafts();
  let saved: ResearchVariableDefinition | undefined;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method !== "research.variable.register") throw new Error("unexpected mutation");
    const body = params as {
      id: string;
      registration: {
        id: string;
        name: string;
        purpose: string;
        expression: string;
        window: number;
      };
    };
    saved = {
      ...body.registration,
      object: {
        domain: "research",
        kind: "research_variable",
        node: "owner",
        service: "research",
        id: body.registration.id,
        version: "a".repeat(64),
      },
      price_power: 0,
      required_history: 2,
      recorded_at_ms: "1",
    };
    if (trade.mock.calls.length === 1)
      throw new Error("reply unavailable after durable registration");
  });
  const query: TerminalContext["query"] = async (...[method]) => ({
    ...snapshot,
    research_variable: {
      definitions: saved ? [saved] : [],
      next_after: "",
      ...(method === "research.variable.read" ? { definition: saved } : {}),
      ...(method === "research.variable.check"
        ? {
            preview: {
              valid: true,
              price_power: 0,
              required_history: 2,
              diagnostic: "",
              error_offset: null,
            },
          }
        : {}),
    },
  });
  const context: TerminalContext = {
    snapshot,
    query,
    trade,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
  };
  const mounted = render(
    <WorkspaceDraftScope store={store} namespace="variables">
      <ResearchVariables {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.change(screen.getByLabelText("变量名称"), { target: { value: "original" } });
  fireEvent.change(screen.getByLabelText("研究用途"), {
    target: { value: "causal synthetic evidence" },
  });
  fireEvent.click(screen.getByRole("button", { name: "检查表达式" }));
  await waitFor(() =>
    expect(
      (screen.getByRole("button", { name: "登记不可变定义" }) as HTMLButtonElement).disabled,
    ).toBe(false),
  );
  fireEvent.click(screen.getByRole("button", { name: "登记不可变定义" }));
  await waitFor(() => expect(screen.getByRole("alert")).toBeTruthy());
  fireEvent.click(screen.getByRole("button", { name: "登记不可变定义" }));
  await waitFor(() =>
    expect(screen.getByRole("button", { name: "基于此定义创建新草稿" })).toBeTruthy(),
  );
  expect(trade).toHaveBeenCalledTimes(2);
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  expect(screen.getByLabelText("表达式").matches(":disabled")).toBe(true);
  expect(screen.getByText("original · causal synthetic evidence")).toBeTruthy();
  const clean = new Event("beforeunload", { cancelable: true });
  window.dispatchEvent(clean);
  expect(clean.defaultPrevented).toBe(false);
  mounted.unmount();
  store.dispose();
});
