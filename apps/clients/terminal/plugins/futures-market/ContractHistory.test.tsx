import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { Snapshot, HistoryDatasetRecord } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { ContractHistory } from "./ContractHistory";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
vi.mock("@asterion/client-ui/PriceChart", () => ({
  PriceChart: () => <output aria-label="绘制数据" />,
}));
const contract = "SHFE/rb/2026-10";
const daily = {
  snapshot: "c".repeat(64),
  daily_source: "tushare.fut_daily.v3",
  calendar_source: "tushare.trade_cal.v1",
  contracts_source: "tushare.fut_basic.v1",
};
const versions: HistoryDatasetRecord[] = ["a", "b"].map(c => ({
  id: c.repeat(64),
  contract_id: contract,
  source: "tushare.ft_mins",
  revision: "1",
  begin: "2026-09-01",
  end: "2026-09-02",
  interval_minutes: 1,
  rows: 120,
}));
function fixture() {
  const snapshot = readySnapshot();
  const query = vi.fn<TerminalContext["query"]>(async (method, p = {}) => {
    if (method === "data.datasets") return { ...snapshot, history_datasets: versions };
    const offset = p.offset as number,
      limit = p.limit as number;
    const bars = Array.from({ length: Math.max(0, Math.min(limit, 120 - offset)) }, (_, i) => ({
      timestamp_ns: String(1790000000000000000n + BigInt(offset + i) * 60000000000n),
      trading_day: new Date(Date.UTC(2026, 0, offset + i + 1)).toISOString().slice(0, 10),
      open: "100",
      high: "101",
      low: "99",
      close: "100",
      volume: "1",
      amount: "100",
      open_interest: "1",
    }));
    if (method === "data.tables.daily")
      return {
        ...snapshot,
        table_daily: {
          daily: p.daily as typeof daily,
          contract: p.contract as string,
          period: "day",
          total: 120,
          offset,
          limit,
          candles: bars,
        },
      };
    return {
      ...snapshot,
      history_page: {
        id: p.id as string,
        source: "tushare.ft_mins",
        contract_id: contract,
        interval_minutes: 1,
        manifest_sha256: "d".repeat(64),
        total_rows: 120,
        matched_rows: 120,
        offset,
        limit,
        first_ns: "1",
        last_ns: "2",
        begin_ns: "1",
        end_ns: "2",
        bars,
      },
    };
  });
  const context: TerminalContext = {
    snapshot,
    query,
    busy: false,
    error: "",
    trade: vi.fn(async () => {}),
    navigate: vi.fn(),
    openSettings: vi.fn(),
  };
  return { snapshot, query, context };
}
function page(
  context: TerminalContext,
  store: ReturnType<typeof createWorkspaceDrafts>,
  period: "day" | 1,
  table = daily,
) {
  return (
    <WorkspaceDraftScope store={store} namespace="market">
      <ContractHistory
        venue="SHFE"
        symbol={contract}
        historyContractId={contract}
        dailyTable={table}
        fixedPeriod={period}
        context={context}
        preferenceKey="resume-test"
      />
    </WorkspaceDraftScope>
  );
}
const pages = () => document.querySelector(".market-history-pages")!;
test("daily page survives leaving the workspace and repeated returns; a new snapshot starts at zero", async () => {
  const f = fixture(),
    store = createWorkspaceDrafts();
  let view = render(page(f.context, store, "day"));
  await screen.findByLabelText("绘制数据");
  fireEvent.click(screen.getByRole("button", { name: "向后" }));
  await waitFor(() => expect(pages().textContent).toContain("101–120 / 120"));
  for (let i = 0; i < 2; i++) {
    view.unmount();
    view = render(page(f.context, store, "day"));
    await waitFor(() => expect(pages()?.textContent).toContain("101–120 / 120"));
    expect(f.query.mock.calls.filter(([m]) => m === "data.tables.daily").at(-1)?.[1]?.offset).toBe(
      41,
    );
  }
  view.unmount();
  render(page(f.context, store, "day", { ...daily, snapshot: "e".repeat(64) }));
  await waitFor(() => expect(pages()?.textContent).toContain("1–100 / 120"));
  expect(f.context.trade).not.toHaveBeenCalled();
});
test("minute dataset version and page survive navigation; a new service cannot inherit them", async () => {
  const f = fixture(),
    store = createWorkspaceDrafts();
  const view = render(page(f.context, store, 1));
  await screen.findByLabelText("绘制数据");
  fireEvent.change(screen.getByLabelText("历史数据版本"), { target: { value: versions[1].id } });
  await waitFor(() =>
    expect((screen.getByLabelText("历史数据版本") as HTMLSelectElement).value).toBe(versions[1].id),
  );
  fireEvent.click(screen.getByRole("button", { name: "向后" }));
  await waitFor(() => expect(pages()?.textContent).toContain("101–120 / 120"));
  view.unmount();
  const back = render(page(f.context, store, 1));
  await waitFor(() => expect(pages()?.textContent).toContain("101–120 / 120"));
  expect((screen.getByLabelText("历史数据版本") as HTMLSelectElement).value).toBe(versions[1].id);
  back.unmount();
  const next: Snapshot = {
    ...f.snapshot,
    data: { ...f.snapshot.data!, connection_id: "new-service" },
  };
  render(page({ ...f.context, snapshot: next }, store, 1));
  await waitFor(() => expect(pages()?.textContent).toContain("1–100 / 120"));
  expect((screen.getByLabelText("历史数据版本") as HTMLSelectElement).value).toBe(versions[0].id);
  expect(f.context.trade).not.toHaveBeenCalled();
});
