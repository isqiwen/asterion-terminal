import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { LogicDefinition, LogicExample } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { ResearchLogic } from "./ResearchLogic";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
test("lost prior-example receipt retries the same immutable case and never starts a task automatically", async () => {
  const snapshot = readySnapshot();
  if (!snapshot.research_service) throw new Error("missing fixture service");
  snapshot.research_service.owner_node_id = "fixture-node";
  const object = {
    node: "fixture-node",
    service: snapshot.research_service.service,
    domain: "research" as const,
    kind: "logic_definition",
    id: "means",
    version: "a".repeat(64),
  };
  const definition: LogicDefinition = {
    object,
    name: "Saved means",
    purpose: "Synthetic only",
    fast: 1,
    slow: 3,
    input: "completed_close",
    output: "target_sign",
    recorded_at_ms: "1",
    retirement_reason: "",
  };
  let saved: LogicExample | undefined;
  let saves = 0;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method === "research.logic.example.save") {
      saved = {
        object: { ...object, kind: "logic_case", id: params.example.id, version: "b".repeat(64) },
        ...params.example,
        recorded_at_ms: "2",
      };
      if (++saves === 1) throw new Error("reply lost after saved prior expectations");
    } else if (method !== "research.logic.trial") throw new Error("unexpected mutation");
  });
  const query: TerminalContext["query"] = async (...[method]) => ({
    ...snapshot,
    research_logic: {
      definitions: method === "research.logic.list" ? [definition] : [],
      examples: saved ? [saved] : [],
      definition: method === "research.logic.read" ? definition : undefined,
      example: method === "research.logic.example.read" ? saved : undefined,
      next_after: "",
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
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="logic">
      <ResearchLogic {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "Saved means" }));
  fireEvent.change(await screen.findByLabelText("样例名称", { exact: true }), {
    target: { value: "Prior expectations" },
  });
  fireEvent.change(screen.getByLabelText("适用范围", { exact: true }), {
    target: { value: "Fictional causal logic only" },
  });
  fireEvent.click(screen.getByRole("button", { name: "保存事先预期，不运行" }));
  await screen.findByRole("alert");
  fireEvent.click(screen.getByRole("button", { name: "保存事先预期，不运行" }));
  await screen.findByRole("heading", { name: "Prior expectations" });
  expect(trade).toHaveBeenCalledTimes(2);
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  expect(screen.queryByLabelText("虚构收盘与事先预期", { exact: true })).toBeNull();
  expect(saved?.observations[2].expected).toEqual({ state: "target_sign", value: 1 });
  const original = JSON.stringify(saved);
  fireEvent.click(screen.getByRole("button", { name: "运行固定样例试算" }));
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(3));
  const [method, params] = trade.mock.calls[2];
  expect(method).toBe("research.logic.trial");
  if (method !== "research.logic.trial") throw new Error("wrong mutation");
  expect(params.example).toEqual(saved?.object);
  expect(JSON.stringify(saved)).toBe(original);
});
