import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { TerminalContext } from "../contract";
import { ResearchSamples } from "./ResearchSamples";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
test("uncertain sample submission retains exact selection and owner across source changes and offline replay", async () => {
  const snapshot = readySnapshot();
  snapshot.data!.owner_node_id = "stable-data-node";
  snapshot.datasets = [
    {
      source_dataset_ids: ["minute-original"],
      daily: {
        snapshot: "a".repeat(64),
        daily_source: "daily",
        calendar_source: "calendar",
        contracts_source: "contracts",
      },
      begin_day: "2026-09-25",
      end_day: "2026-09-25",
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
      price_increment_changes: [],
      venue: "SHFE",
      symbol: "rb2610",
      revision: "original",
      source: "fixture",
      interval_minutes: 1,
      count: 6,
      days: 1,
      first_day: "2026-09-25",
      last_day: "2026-09-25",
      first_timestamp_ns: "1",
      last_timestamp_ns: "6",
      last_close: "4",
      uncovered_days: [],
    },
  ];
  const trade = vi.fn<TerminalContext["trade"]>(async () => {
    throw new Error("reply unavailable after admission");
  });
  const query = vi.fn(async () => snapshot);
  const context: TerminalContext = {
    snapshot,
    trade,
    query,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
  };
  const drafts = createWorkspaceDrafts();
  const view = () => (
    <WorkspaceDraftScope store={drafts} namespace="sample-test">
      <ResearchSamples {...context} />
    </WorkspaceDraftScope>
  );
  const mounted = render(view());
  fireEvent.click(screen.getByRole("button", { name: "固定当前数据选择" }));
  fireEvent.change(screen.getByLabelText("样本用途"), {
    target: { value: "synthetic post-hoc distribution" },
  });
  fireEvent.click(screen.getByLabelText("划分留出区间"));
  fireEvent.change(screen.getByLabelText("首个留出观察索引"), { target: { value: "4" } });
  const submit = screen.getByRole("button", { name: "提交或确认原样本任务" });
  expect(submit.matches(":disabled")).toBe(true);
  fireEvent.click(
    screen.getByLabelText("确认仅作事后探索：历史可得时间未知，完整历史覆盖未获证明。"),
  );
  fireEvent.click(submit);
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(1));
  await waitFor(() => expect(submit.matches(":disabled")).toBe(false));
  snapshot.datasets[0].source_dataset_ids = ["minute-changed"];
  snapshot.data!.owner_node_id = "different-data-node";
  snapshot.data!.online = false;
  mounted.rerender(view());
  fireEvent.click(screen.getByRole("button", { name: "提交或确认原样本任务" }));
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(2));
  expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
  expect(trade.mock.calls[0]).toEqual([
    "research.sample.submit",
    expect.objectContaining({
      data_owner: { node_id: "stable-data-node", service_id: "historical-data" },
      selection: expect.objectContaining({
        source_dataset_ids: ["minute-original"],
        contract: "SHFE/rb/2026-10",
      }),
      mode: "post_hoc",
      evaluation: { mode: "holdout", split_index: 4 },
    }),
  ]);
  expect(query).not.toHaveBeenCalled();
});
