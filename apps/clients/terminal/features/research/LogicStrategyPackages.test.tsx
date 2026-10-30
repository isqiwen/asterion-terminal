import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { LogicStrategyPackage, LogicStrategyDelivery } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { LogicStrategyPackages } from "./LogicStrategyPackages";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
test("logic delivery retries its original draft and only navigates to explicit backtest configuration", async () => {
  const snapshot = readySnapshot();
  if (!snapshot.research_service) throw new Error("missing fixture service");
  snapshot.research_service.owner_node_id = "fixture-node";
  const object = {
    node: "fixture-node",
    service: snapshot.research_service.service,
    domain: "research" as const,
    kind: "strategy_package",
    id: "logic-package",
    version: "a".repeat(64),
  };
  const candidate: LogicStrategyPackage = {
    object,
    name: "Checked fictional rule",
    purpose: "Research only",
    limitations: "Not market performance or live permission",
    evidence_relation: "logic_trial",
    reference_result: null,
    definition: {
      strategy: {
        quantity: "1",
        sides: "both",
        rule: { kind: "moving_average", fast: 1, slow: 3 },
      },
      contract: {
        venue: "SHFE",
        symbol: "rb2610",
        product: "rb",
        delivery_month: "2026-10",
        currency: "CNY",
        price_increment: "1",
        quantity_increment: "1",
        multiplier: "10",
      },
      interval_minutes: 1,
      program_artifact: "b".repeat(64),
    },
    logic: {
      definition: {
        object: { ...object, kind: "logic_definition", id: "means", version: "d".repeat(64) },
        name: "Original means",
        purpose: "Prior fictional check",
        fast: 1,
        slow: 3,
        input: "completed_close",
        output: "target_sign",
        recorded_at_ms: "1",
        retirement_reason: "",
      },
      example: { ...object, kind: "logic_case", id: "prior-case", version: "e".repeat(64) },
      result: { ...object, kind: "logic_trial_result", id: "trial", version: "f".repeat(64) },
    },
    source_input_sha256: "1".repeat(64),
    source_attempt: 1,
    recorded_at_ms: "2",
    retirement_reason: "",
  };
  let saved: LogicStrategyDelivery | undefined;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method !== "research.strategy.delivery.create")
      throw new Error("unexpected execution or mutation");
    saved = {
      object: {
        ...object,
        kind: "strategy_delivery",
        id: params.delivery.id,
        version: "2".repeat(64),
      },
      package: params.delivery.package,
      reference_result: null,
      logic_trial: candidate.logic.result,
      target_task_id: params.delivery.target_task_id,
      source_input_sha256: candidate.source_input_sha256,
      recorded_at_ms: "3",
    };
    if (trade.mock.calls.length === 1) throw new Error("lost durable draft reply");
  });
  const query: TerminalContext["query"] = async (...[method]) => ({
    ...snapshot,
    research_strategy: {
      packages: method === "research.strategy.list" ? [candidate] : [],
      deliveries: saved ? [saved] : [],
      package: method === "research.strategy.read" ? candidate : undefined,
      delivery: method === "research.strategy.delivery.read" ? saved : undefined,
      target_accepted: method === "research.strategy.delivery.read" ? false : undefined,
      next_after: "",
    },
  });
  const navigate = vi.fn();
  const context: TerminalContext = {
    snapshot,
    query,
    trade,
    navigate,
    busy: false,
    error: "",
    openSettings: vi.fn(),
  };
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="logic-delivery">
      <LogicStrategyPackages {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(
    await screen.findByRole("button", { name: "Checked fictional rule · logic-package" }),
  );
  await screen.findByRole("region", { name: "固定逻辑包定义" });
  fireEvent.click(screen.getByRole("button", { name: "保存交付草稿，不运行" }));
  await screen.findByRole("alert");
  fireEvent.click(screen.getByRole("button", { name: "保存交付草稿，不运行" }));
  await screen.findByText("草稿已保存，尚无正式接纳");
  expect(trade).toHaveBeenCalledTimes(2);
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  expect(saved?.reference_result).toBeNull();
  expect(saved?.logic_trial).toEqual(candidate.logic.result);
  expect(navigate).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "配置交付回测草稿" }));
  await waitFor(() =>
    expect(navigate).toHaveBeenCalledWith("workspace.backtest", {
      page: "backtest",
      params: { strategy_delivery_id: saved?.object.id },
    }),
  );
  expect(trade).toHaveBeenCalledTimes(2);
});
