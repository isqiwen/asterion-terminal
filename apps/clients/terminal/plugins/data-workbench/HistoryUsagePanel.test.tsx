import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type {
  HistoryDatasetRecord,
  HistoryReference,
  HistoryUsage,
  Snapshot,
} from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { BackendError, type TerminalContext } from "../contract";
import { HistoryUsagePanel } from "./HistoryUsagePanel";
import { plugin } from "./plugin";

registerTerminalPlugins([plugin]);

const version = (id: string): HistoryDatasetRecord => ({
  id,
  contract_id: "al2601.SHFE",
  source: "tushare",
  revision: `revision-${id}`,
  begin: "2026-01-05",
  end: "2026-01-09",
  interval_minutes: 1,
  rows: 100,
});
// What the core answers for one version: a saved dataset uses it in `roles`.
const usedBy = (dataset: string, roles: HistoryReference["roles"]): Snapshot => {
  const usage: HistoryUsage = {
    dataset_id: dataset,
    references: [{ kind: "saved_dataset", id: "saved-1", name: "引用核对组合", roles }],
    selected_roles: [],
    disconnected_nodes: { names: [] },
    other_data_services: [],
  };
  return { ...readySnapshot(), history_usage: usage };
};
const panel = (query: TerminalContext["query"], id: string) => (
  <HistoryUsagePanel
    context={{
      snapshot: null,
      busy: false,
      error: "",
      navigate: () => {},
      openSettings: () => {},
      query,
      trade: async () => {},
    }}
    item={version(id)}
    onClose={() => {}}
  />
);
const records = () => screen.getByRole("table", { name: "数据版本关联记录" }).textContent;

test("a reply for a version that is no longer shown is ignored", async () => {
  let release!: (reply: Snapshot) => void;
  const held = new Promise<Snapshot>(resolve => {
    release = resolve;
  });
  const query = vi.fn(async (_method: string, params: Record<string, unknown>) =>
    params.id === "minute" ? held : usedBy("daily", ["settlement"]),
  );
  const view = render(panel(query, "minute"));
  await waitFor(() => expect(query).toHaveBeenCalledTimes(1));
  view.rerender(panel(query, "daily"));
  await waitFor(() => expect(records()).toContain("结算输入"));
  await act(async () => release(usedBy("minute", ["market"])));
  expect(records()).toContain("结算输入");
  expect(records()).not.toContain("行情输入");
});

test("a failed check shows its error and neither an earlier result nor an all-clear", async () => {
  let failing = false;
  const query = async () => {
    if (failing) throw new BackendError("unavailable", "invalid historical usage response");
    return usedBy("minute", ["market"]);
  };
  render(panel(query, "minute"));
  await waitFor(() => expect(records()).toContain("行情输入"));
  failing = true;
  fireEvent.click(screen.getByRole("button", { name: "刷新使用情况" }));
  await screen.findByRole("alert");
  expect(screen.queryByRole("table")).toBeNull();
  expect(screen.getByRole("region", { name: "使用情况" }).textContent).not.toContain(
    "未发现关联记录",
  );
});
