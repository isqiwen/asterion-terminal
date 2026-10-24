import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { NoticeObject, NoticeSourceObservation, Snapshot } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { NoticeSourceChanges } from "./NoticeSourceChanges";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const source: NoticeObject = {
  node: "stable-node",
  service: "intelligence-one",
  domain: "intelligence",
  kind: "intelligence_publication",
  id: "original-publication",
  version: "a".repeat(64),
};
const input = {
  id: "fixed-audit",
  input_sha256: "b".repeat(64),
  source,
  owner: { node: "stable-node", service: "research" },
};
const event = {
  reference: { ...source, kind: "intelligence_event", id: "event-one" },
  initial_publication: source,
  contract: "SHFE/rb/2026-11",
  rule_id: "contract_margin_announcement",
  use_scope: "research",
  available_at_ms: "100",
};
const head = { ...source, kind: "intelligence_change", id: "withdrawal" };
const observation: NoticeSourceObservation = {
  consumer: {
    ...input.owner,
    domain: "research",
    kind: "notice_review",
    id: input.id,
    version: input.input_sha256,
  },
  source,
  current: {
    event,
    head,
    selected_publication: source,
    withdrawn: true,
    pending_change: null,
    latest_change: {
      reference: { ...head, kind: "intelligence_change_preparation" },
      event: event.reference,
      expected_head: source,
      action: "withdraw",
      replacement: null,
      material: {
        ...source,
        domain: "data",
        service: "data-one",
        kind: "material_acquisition",
        id: "withdrawal-evidence",
      },
      evidence: { begin: 0, end: 9, text: "withdrawn" },
      operator_name: "human",
      reason: "The new original withdraws the announcement",
      effective_at_ms: null,
      affected_publication: source,
      reviewed_at_ms: "200",
      prepared_at_ms: "200",
      program_artifact: "c".repeat(64),
      published: {
        reference: head,
        content: {
          ...head,
          domain: "data",
          service: "data-one",
          kind: "intelligence_change_content",
        },
        stored_at_ms: "210",
        available_at_ms: "220",
        sequence: "1",
      },
    },
  },
  observed_at_ms: "300",
};
function context(): TerminalContext {
  const snapshot = readySnapshot();
  snapshot.data = null;
  snapshot.intelligence_service = null;
  snapshot.research_service!.owner_node_id = "stable-node";
  return {
    snapshot,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
    query: vi.fn(async () => ({ ...snapshot, notice_source_observation: observation })),
    trade: vi.fn(async () => {}),
  };
}
test("fixed Research source inspection needs neither a selected Data nor Intelligence and never changes the original input", async () => {
  const ctx = context(),
    original = JSON.stringify(input);
  render(<NoticeSourceChanges context={ctx} input={input} />);
  expect(ctx.query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "核查来源后继变化" }));
  await screen.findByText("源事件当前已撤回");
  expect(ctx.query).toHaveBeenCalledWith("notice.source.inspect", {
    id: input.id,
    owner: {
      kind: "research",
      node_id: "local",
      service: "research",
      connection_id: "research-connection",
    },
  });
  expect(ctx.trade).not.toHaveBeenCalled();
  expect(JSON.stringify(input)).toBe(original);
  expect(screen.getByText(/不是原历史结果或持续在线证明/)).toBeTruthy();
  ctx.query = vi.fn(async () => {
    throw Error("offline");
  });
  fireEvent.click(screen.getByRole("button", { name: "核查来源后继变化" }));
  await screen.findByText(/来源当前状态未知/);
  expect(screen.queryByText("源事件当前已撤回")).toBeNull();
  expect(screen.queryByText("当前未见已发布后继变化")).toBeNull();
});
test("an observation for another fixed input is rejected rather than displayed as this audit's current state", async () => {
  const ctx = context();
  ctx.query = vi.fn(async () => ({
    ...ctx.snapshot!,
    notice_source_observation: {
      ...observation,
      consumer: { ...observation.consumer, version: "d".repeat(64) },
    },
  }));
  render(<NoticeSourceChanges context={ctx} input={input} />);
  fireEvent.click(screen.getByRole("button", { name: "核查来源后继变化" }));
  await screen.findByText(/来源变化观察响应不符/);
  expect(screen.queryByText("源事件当前已撤回")).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
});
test("a response arriving after Research owner selection changes is discarded", async () => {
  const ctx = context();
  let finish!: (value: Snapshot) => void;
  ctx.query = vi.fn(
    () =>
      new Promise<Snapshot>(resolve => {
        finish = resolve;
      }),
  );
  const page = render(<NoticeSourceChanges context={ctx} input={input} />);
  fireEvent.click(screen.getByRole("button", { name: "核查来源后继变化" }));
  const next = {
    ...ctx,
    snapshot: {
      ...ctx.snapshot!,
      research_service: {
        ...ctx.snapshot!.research_service!,
        connection_id: "new-selection",
      },
    },
  };
  page.rerender(<NoticeSourceChanges context={next} input={input} />);
  await act(async () => finish({ ...ctx.snapshot!, notice_source_observation: observation }));
  await waitFor(() => expect(screen.queryByText("源事件当前已撤回")).toBeNull());
  expect(ctx.trade).not.toHaveBeenCalled();
});
