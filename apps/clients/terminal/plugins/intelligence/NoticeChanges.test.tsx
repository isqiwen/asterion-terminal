import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type {
  NoticeObject,
  NoticeRecord,
  NoticeEventView,
  NoticeChangeRecord,
  Snapshot,
} from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { NoticeChanges } from "./NoticeChanges";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const object = (kind: string, id: string): NoticeObject => ({
  node: "stable-node",
  service: "intelligence-one",
  domain: "intelligence",
  kind,
  id,
  version: "a".repeat(64),
});
const initial = object("intelligence_publication", "original-publication");
const event = {
  reference: object("intelligence_event", "event-one"),
  initial_publication: initial,
  contract: "SHFE/rb/2026-11",
  use_scope: "research",
  rule_id: "contract_margin_announcement",
  available_at_ms: "100",
};
const span = { begin: 0, end: 3, text: "new" };
const pendingChange: NoticeChangeRecord = {
  reference: object("intelligence_change_preparation", "original-change-operation"),
  event: event.reference,
  expected_head: initial,
  action: "withdraw",
  replacement: null,
  material: {
    ...object("material_acquisition", "new-evidence"),
    domain: "data",
    service: "data-one",
  },
  evidence: span,
  operator_name: "original human",
  reason: "the new original withdraws this statement",
  effective_at_ms: null,
  affected_publication: initial,
  reviewed_at_ms: "200",
  prepared_at_ms: "200",
  program_artifact: "b".repeat(64),
  published: null,
};
const record: NoticeRecord = {
  reference: object("intelligence_candidate", "candidate-one"),
  state: "published",
  material: {
    reference: pendingChange.material,
    content: { ...pendingChange.material, kind: "material_content" },
    request: {
      id: "new-evidence",
      source_id: "fixture",
      resource_id: "notice",
      use_scope: "research",
      provider_artifact: "b".repeat(64),
      max_requests: 1,
      max_bytes: 4096,
    },
    media_type: "text/plain",
    bytes: 3,
    acquired_at_ms: "90",
    source_published_at_ms: null,
  },
  dictionary_reference: null,
  contract: event.contract,
  evidence: {
    subject: span,
    parameter: span,
    effective_day: span,
    percentage: span,
    modality: span,
  },
  rule_id: event.rule_id,
  use_scope: event.use_scope,
  use_mode: "post_hoc_research",
  program_artifact: "b".repeat(64),
  created_at_ms: "90",
  updated_at_ms: "100",
  assessment: {
    checks: [],
    limitations: [],
    ready_for_human_review: true,
    effective_day: "2026-10-24",
    margin_ratio: "0.12",
  },
  verification: null,
  published: {
    reference: initial,
    preparation: object("intelligence_preparation", "original-publication"),
    content: pendingChange.material,
    stored_at_ms: "99",
    available_at_ms: "100",
  },
  event,
};
const current: NoticeEventView = {
  event,
  head: initial,
  selected_publication: initial,
  withdrawn: false,
  latest_change: null,
  pending_change: pendingChange,
};
function context(): TerminalContext {
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
    query: vi.fn(async () => ({ ...snapshot, notice_event_view: current })),
    trade: vi.fn(async () => {}),
  };
}
test("current pending intent resumes the original human request after a lost reply without inventing an operation", async () => {
  const ctx = context();
  let attempts = 0;
  ctx.trade = vi.fn(async () => {
    if (++attempts === 1) throw Error("offline");
  });
  render(<NoticeChanges context={ctx} record={record} />);
  expect(ctx.query).not.toHaveBeenCalled();
  expect(ctx.trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "读取事件状态" }));
  await screen.findByText(/original-change-operation/);
  fireEvent.click(screen.getByRole("button", { name: "续办原变化发布" }));
  await waitFor(() =>
    expect(
      (screen.getByRole("button", { name: "续办原变化发布" }) as HTMLButtonElement).disabled,
    ).toBe(false),
  );
  fireEvent.click(screen.getByRole("button", { name: "续办原变化发布" }));
  await waitFor(() => expect(ctx.trade).toHaveBeenCalledTimes(2));
  const expected = {
    request: {
      id: pendingChange.reference.id,
      event: event.reference,
      expected_head: initial,
      action: "withdraw",
      replacement: null,
      material: pendingChange.material,
      evidence: span,
      operator_name: pendingChange.operator_name,
      reason: pendingChange.reason,
      human_confirmed: true,
      effective_at_ms: null,
    },
  };
  expect(ctx.trade).toHaveBeenNthCalledWith(1, "intelligence.change", expected);
  expect(ctx.trade).toHaveBeenNthCalledWith(2, "intelligence.change", expected);
});
test("historical views keep the original statement and cannot authorize a new change", async () => {
  const ctx = context();
  ctx.query = vi.fn(async () => ({
    ...ctx.snapshot!,
    notice_event_view: { ...current, pending_change: null },
  }));
  render(<NoticeChanges context={ctx} record={record} />);
  fireEvent.change(screen.getByLabelText("截至时间（UTC 毫秒，留空查询当前）"), {
    target: { value: "150" },
  });
  fireEvent.click(screen.getByRole("button", { name: "读取事件状态" }));
  await screen.findByText(/截至时点状态/);
  expect(ctx.query).toHaveBeenCalledWith("intelligence.event.view", {
    event: event.reference,
    as_of_ms: "150",
  });
  expect(
    (screen.getByRole("button", { name: "保存并追加变化" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(ctx.trade).not.toHaveBeenCalled();
});
test("changing the selected owner discards a pending old-owner event response", async () => {
  const ctx = context();
  let finish!: (value: Snapshot) => void;
  ctx.query = vi.fn(
    () =>
      new Promise<Snapshot>(resolve => {
        finish = resolve;
      }),
  );
  const page = render(<NoticeChanges context={ctx} record={record} />);
  fireEvent.click(screen.getByRole("button", { name: "读取事件状态" }));
  const next = {
    ...ctx,
    snapshot: {
      ...ctx.snapshot!,
      intelligence_service: {
        ...ctx.snapshot!.intelligence_service!,
        connection_id: "selected-two",
      },
    },
  };
  page.rerender(<NoticeChanges context={next} record={record} />);
  await act(async () => finish({ ...ctx.snapshot!, notice_event_view: current }));
  expect(screen.queryByText(/原变化待确认/)).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
});

test("switching statements within one event discards an old statement's pending form response", async () => {
  const ctx = context();
  let finish!: (value: Snapshot) => void;
  ctx.query = vi.fn(
    () =>
      new Promise<Snapshot>(resolve => {
        finish = resolve;
      }),
  );
  const page = render(<NoticeChanges context={ctx} record={record} />);
  fireEvent.click(screen.getByRole("button", { name: "读取事件状态" }));
  const replacement = {
    ...record,
    published: {
      ...record.published!,
      reference: object("intelligence_publication", "replacement-statement"),
    },
  };
  page.rerender(<NoticeChanges context={ctx} record={replacement} />);
  await act(async () => finish({ ...ctx.snapshot!, notice_event_view: current }));
  expect(screen.queryByText(/原变化待确认/)).toBeNull();
  expect(
    (screen.getByRole("button", { name: "保存并追加变化" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(ctx.trade).not.toHaveBeenCalled();
});
