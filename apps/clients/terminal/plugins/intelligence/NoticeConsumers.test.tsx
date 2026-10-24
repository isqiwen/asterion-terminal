import { act, fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { NoticeObject, NoticeConsumerPage, Snapshot } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { NoticeConsumers } from "./NoticeConsumers";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const source: NoticeObject = {
  node: "stable-node",
  service: "intelligence-one",
  domain: "intelligence",
  kind: "intelligence_publication",
  id: "publication-one",
  version: "a".repeat(64),
};
const page: NoticeConsumerPage = {
  source,
  next_after: "",
  items: (["prepared", "confirmed", "released"] as const).map((phase, i) => ({
    source,
    consumer: {
      node: source.node,
      service: "research-one",
      domain: "research",
      kind: "notice_review",
      id: ["preparing-ref", "held-task", "released-ref"][i],
      version: "b".repeat(64),
    },
    use_scope: "research",
    phase,
    created_at_ms: "1",
    updated_at_ms: "2",
  })),
};
function context(): TerminalContext {
  const snapshot = readySnapshot();
  snapshot.intelligence_service = {
    node_id: "local",
    owner_node_id: source.node,
    service: source.service,
    connection_id: "intel-connection",
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
    query: vi.fn(async () => ({ ...snapshot, notice_consumers: page })),
    trade: vi.fn(async () => {}),
  };
}
test("source page distinguishes held preparations, confirmed relations and durable release without deletion", async () => {
  const ctx = context();
  render(<NoticeConsumers context={ctx} source={source} />);
  expect(ctx.query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "读取源引用登记" }));
  await screen.findByText(/held-task/);
  expect(screen.getByText("已保留，待关系确认")).toBeTruthy();
  expect(screen.getByText("引用关系已确认")).toBeTruthy();
  expect(screen.getByText("消费者已持久解除")).toBeTruthy();
  expect(screen.queryByRole("button", { name: /删除|解除/ })).toBeNull();
  expect(ctx.query).toHaveBeenCalledWith("intelligence.consumers", { source, after: "" });
  expect(ctx.trade).not.toHaveBeenCalled();
});
test("a mismatched source is an error and never an empty or released reference set", async () => {
  const ctx = context();
  ctx.query = vi.fn(async () => ({
    ...ctx.snapshot!,
    notice_consumers: { ...page, source: { ...source, version: "f".repeat(64) } },
  }));
  render(<NoticeConsumers context={ctx} source={source} />);
  fireEvent.click(screen.getByRole("button", { name: "读取源引用登记" }));
  await screen.findByText(/源引用登记响应不符/);
  expect(screen.queryByText(/held-task/)).toBeNull();
  expect(screen.queryByText(/当前登记页为空/)).toBeNull();
});
test("changing the selected Intelligence owner discards an in-flight old page", async () => {
  const ctx = context();
  let finish!: (value: Snapshot) => void;
  ctx.query = vi.fn(
    () =>
      new Promise<Snapshot>(resolve => {
        finish = resolve;
      }),
  );
  const view = render(<NoticeConsumers context={ctx} source={source} />);
  fireEvent.click(screen.getByRole("button", { name: "读取源引用登记" }));
  const changed = {
    ...ctx,
    snapshot: {
      ...ctx.snapshot!,
      intelligence_service: { ...ctx.snapshot!.intelligence_service!, connection_id: "new-owner" },
    },
  };
  view.rerender(<NoticeConsumers context={changed} source={source} />);
  await act(async () => {
    finish({ ...ctx.snapshot!, notice_consumers: page });
  });
  expect(screen.queryByText(/held-task/)).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
});
