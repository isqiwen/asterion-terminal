import { fireEvent, render, screen, waitFor, act } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { MaterialRead, NoticeRecord, Snapshot } from "../../src/bridge/client";
import type { CommandCall } from "../../src/bridge/commands";
import type { QueryCommand } from "../contract";
import { NoticeReview } from "./NoticeReview";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const source = {
  node: "stable-node",
  service: "data-one",
  domain: "data" as const,
  kind: "material_acquisition",
  id: "material-one",
  version: "a".repeat(64),
};
const material: MaterialRead = {
  receipt: {
    reference: source,
    content: { ...source, kind: "material_content", id: "b".repeat(64), version: "b".repeat(64) },
    request: {
      id: source.id,
      source_id: "synthetic",
      resource_id: "notice",
      use_scope: "research",
      provider_artifact: "c".repeat(64),
      max_requests: 1,
      max_bytes: 4096,
    },
    media_type: "text/plain",
    bytes: 88,
    acquired_at_ms: "1790000000000",
    source_published_at_ms: null,
  },
  reading: { state: "readable", readable_bytes: 88 },
  text: "SHFE/rb/2026-11 计划于2026-10-24将保证金率调整为12%。",
};
function record(): NoticeRecord {
  const span = { begin: 0, end: 1, text: "x" };
  return {
    reference: {
      ...source,
      domain: "intelligence",
      service: "intelligence-one",
      kind: "intelligence_candidate",
      id: "candidate-one",
      version: "d".repeat(64),
    },
    state: "draft",
    material: material.receipt,
    dictionary_reference: null,
    contract: "SHFE/rb/2026-11",
    evidence: {
      subject: span,
      parameter: span,
      effective_day: span,
      percentage: span,
      modality: span,
    },
    rule_id: "contract_margin_announcement",
    use_scope: "research",
    use_mode: "post_hoc_research",
    program_artifact: "e".repeat(64),
    created_at_ms: "1790000000001",
    updated_at_ms: "1790000000001",
    assessment: {
      checks: [{ field: "rule", state: "pass", reason: "notice_manual_rule_selected" }],
      limitations: ["notice_semantic_context_requires_human_review"],
      ready_for_human_review: true,
      effective_day: "2026-10-24",
      margin_ratio: "0.12",
    },
    verification: null,
    published: null,
  };
}
function context(r = record()) {
  const snapshot = readySnapshot();
  snapshot.intelligence_service = {
    node_id: "local",
    owner_node_id: "stable-node",
    service: "intelligence-one",
    connection_id: "selected-one",
    remote: false,
    host: "",
    port: 0,
    records: [],
  };
  return {
    snapshot,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
    trade: vi.fn(async () => {}),
    query: vi.fn(async (...args: CommandCall<QueryCommand>): Promise<Snapshot> => ({
      ...snapshot,
      ...(args[0] === "data.material.read"
        ? { material_read: material }
        : { intelligence_record: r }),
    })),
  };
}
async function load() {
  fireEvent.change(screen.getByLabelText("候选 ID"), { target: { value: "candidate-one" } });
  fireEvent.click(screen.getByRole("button", { name: "读取候选状态" }));
  await screen.findByRole("heading", { name: /candidate-one/ });
}
async function human() {
  fireEvent.change(screen.getByLabelText("核验人"), { target: { value: "Synthetic reviewer" } });
  fireEvent.change(screen.getByLabelText("核验理由"), {
    target: { value: "Exact source and planned context reviewed" },
  });
  for (const input of screen.getAllByRole("checkbox")) fireEvent.click(input);
}
test("requires the exact original and all six human fields before an approval command", async () => {
  const ctx = context();
  render(<NoticeReview context={ctx} />);
  await load();
  await human();
  const approve = screen.getByRole("button", { name: "确认核验通过" });
  expect((approve as HTMLButtonElement).disabled).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "读取固定材料" }));
  await screen.findByDisplayValue(material.text!);
  await waitFor(() => expect((approve as HTMLButtonElement).disabled).toBe(false));
  fireEvent.click(screen.getAllByRole("checkbox")[5]);
  expect((approve as HTMLButtonElement).disabled).toBe(true);
  fireEvent.click(screen.getAllByRole("checkbox")[5]);
  fireEvent.click(approve);
  await waitFor(() =>
    expect(ctx.trade).toHaveBeenCalledWith(
      "intelligence.review",
      expect.objectContaining({
        candidate: record().reference,
        approve: true,
        operator_name: "Synthetic reviewer",
        checks: ["subject", "time", "quantity_unit", "modality_context", "source_use", "conflicts"],
      }),
    ),
  );
});
test("human checks cannot override blocked machine evidence", async () => {
  const r = record();
  r.assessment.ready_for_human_review = false;
  r.assessment.checks = [
    { field: "subject_evidence", state: "blocked", reason: "notice_evidence_missing" },
  ];
  const ctx = context(r);
  render(<NoticeReview context={ctx} />);
  await load();
  fireEvent.click(screen.getByRole("button", { name: "读取固定材料" }));
  await screen.findByDisplayValue(material.text!);
  await human();
  expect((screen.getByRole("button", { name: "确认核验通过" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  expect(ctx.trade).not.toHaveBeenCalled();
});
test("an interrupted publication reuses the durable operation and reason", async () => {
  const r = record();
  r.state = "publishing";
  r.publication_request = {
    id: "frozen-publication",
    reason: "Original reviewed publication reason",
  };
  const ctx = context(r);
  render(<NoticeReview context={ctx} />);
  await load();
  expect((screen.getByLabelText("发布理由") as HTMLTextAreaElement).disabled).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "重试保存确认" }));
  await waitFor(() =>
    expect(ctx.trade).toHaveBeenCalledWith("intelligence.publish", {
      id: "frozen-publication",
      candidate: r.reference,
      data_owner: { node: source.node, service: source.service, domain: source.domain },
      reason: "Original reviewed publication reason",
    }),
  );
});
test("a reply from a previous Intelligence selection cannot populate the next owner's page", async () => {
  const ctx = context();
  let resolve!: (value: Snapshot) => void;
  ctx.query.mockImplementation(
    async () =>
      new Promise<Snapshot>(r => {
        resolve = r;
      }),
  );
  const view = render(<NoticeReview context={ctx} />);
  fireEvent.change(screen.getByLabelText("候选 ID"), { target: { value: "candidate-one" } });
  fireEvent.click(screen.getByRole("button", { name: "读取候选状态" }));
  await waitFor(() => expect(ctx.query).toHaveBeenCalled());
  view.rerender(
    <NoticeReview
      context={{
        ...ctx,
        snapshot: {
          ...ctx.snapshot,
          intelligence_service: {
            ...ctx.snapshot.intelligence_service!,
            connection_id: "another-owner",
            service: "other-intelligence",
          },
        },
      }}
    />,
  );
  await act(async () => resolve({ ...ctx.snapshot, intelligence_record: record() }));
  expect(screen.queryByRole("heading", { name: /candidate-one/ })).toBeNull();
});
