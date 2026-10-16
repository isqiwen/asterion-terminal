import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { IntradaySeries, LiveMarket, HistoryDatasetRecord } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { MinuteContinuity } from "./MinuteContinuity";
import { observedRuns, labelBoundary } from "./minute-boundary";
import { plugin } from "./plugin";
vi.mock("./ContractHistory", () => ({
  ContractHistory: ({ fixedMinuteVersion }: { fixedMinuteVersion: string }) => (
    <output aria-label="archive">{fixedMinuteVersion}</output>
  ),
}));
registerTerminalPlugins([plugin]);
const base = Date.UTC(2026, 9, 13, 1);
const ns = (ms: number) => String(BigInt(ms) * 1000000n);
const row: LiveMarket["subscriptions"][number] = {
  venue: "SHFE",
  symbol: "rb2610",
  state: "subscribed",
  error_code: 0,
  quote: null,
};
const market: LiveMarket = {
  ...readySnapshot().market!,
  service: "isolated",
  instance_id: "replay-1",
  transport_online: true,
  phase: "connected",
  subscriptions: [row],
  catalog: {
    ...readySnapshot().market!.catalog,
    contracts: [
      {
        venue: row.venue,
        symbol: row.symbol,
        contract_id: "SHFE/rb/2026-10",
        product: "rb",
        expiry: "20261015",
        multiplier: 10,
        price_tick: "1",
        name: "",
      },
    ],
  },
};
const series: IntradaySeries = {
  service: market.service,
  instance_id: market.instance_id,
  venue: row.venue,
  symbol: row.symbol,
  trading_day: "20261013",
  first_observation_ms: base + 1000,
  interrupted: true,
  previous_settlement: "100",
  bars: [0, 1, 4].map(n => ({
    start_ms: base + n * 60000,
    open: "100",
    high: "103",
    low: "100",
    close: "101",
    volume: 10,
    average_price: "101",
    open_interest: "50",
  })),
};
const versions: HistoryDatasetRecord[] = ["a", "b"].map(c => ({
  id: c.repeat(64),
  revision: c.repeat(64),
  contract_id: "SHFE/rb/2026-10",
  source: "tushare.ft_mins",
  interval_minutes: 1,
  rows: 120,
  begin: "2026-10-12",
  end: "2026-10-13",
}));
function fixture() {
  const snapshot = readySnapshot();
  snapshot.data = { ...snapshot.data!, online: true, connection_id: "isolated-data" };
  const query = vi.fn<TerminalContext["query"]>(async (...[method, p]) => {
    if (method === "data.datasets") return { ...snapshot, history_datasets: versions };
    if (method !== "data.minutes.page") throw new Error(`Unexpected query: ${method}`);
    expect(p.offset).toBe(119);
    expect(p.limit).toBe(1);
    expect(p.include_macd).toBe(false);
    const last = p.id === versions[0].id ? base + 60000 : base - 120000;
    return {
      ...snapshot,
      history_page: {
        id: p.id as string,
        source: "tushare.ft_mins",
        contract_id: "SHFE/rb/2026-10",
        interval_minutes: 1,
        manifest_sha256: p.id as string,
        total_rows: 120,
        matched_rows: 120,
        offset: 119,
        limit: 1,
        begin_ns: ns(last),
        end_ns: ns(last),
        first_ns: ns(last),
        last_ns: ns(last),
        bars: [
          {
            timestamp_ns: ns(last),
            open: "100",
            high: "101",
            low: "99",
            close: "100",
            volume: "1",
            amount: "100",
            open_interest: "1",
          },
        ],
      },
    };
  });
  const trade = vi.fn(async () => {});
  const context: TerminalContext = {
    snapshot,
    query,
    trade,
    busy: false,
    error: "",
    navigate: () => {},
    openSettings: () => {},
  };
  const store = createWorkspaceDrafts();
  const page = (selected = market, observed = series) => (
    <WorkspaceDraftScope store={store} namespace="test">
      <MinuteContinuity
        pane="upper"
        context={context}
        market={selected}
        row={row}
        series={observed}
        error=""
        renderLive={s => <output aria-label="live">{s.bars.map(b => b.start_ms).join(",")}</output>}
      />
    </WorkspaceDraftScope>
  );
  return { page, trade, query };
}
test("actual observed labels form separate runs; ambiguous archive labels never imply candle equivalence", () => {
  expect(observedRuns(series)?.runs.map(r => r.length)).toEqual([2, 1]);
  expect(observedRuns(series)?.gaps).toEqual([
    { after_ms: base + 120000, before_ms: base + 240000, minutes: 2 },
  ]);
  expect(observedRuns({ ...series, bars: [series.bars[0], series.bars[0]] })).toBeNull();
  expect(observedRuns({ ...series, bars: [...series.bars].reverse() })).toBeNull();
  expect(labelBoundary(ns(base), series).kind).toBe("same_label");
  expect(labelBoundary(ns(base + 60000), series).kind).toBe("potential_overlap");
  expect(labelBoundary(ns(base - 120000), series)).toEqual({ kind: "label_gap", gap_minutes: "2" });
  expect(labelBoundary(null, series).kind).toBe("unknown");
});
test("source-exclusive fixed versions and selected runs survive return without appending repeated snapshots", async () => {
  const f = fixture();
  let view = render(f.page());
  await screen.findByText(/标签范围可能重叠/);
  expect(screen.getByLabelText("live").textContent).toBe(String(base + 240000));
  expect(screen.getByText(/未观测标签窗 2/)).toBeTruthy();
  fireEvent.change(screen.getByLabelText("保留观测段"), { target: { value: String(base) } });
  expect(screen.getByLabelText("live").textContent).toBe(`${base},${base + 60000}`);
  view.rerender(f.page(market, { ...series, bars: [...series.bars] }));
  expect(screen.getByLabelText("live").textContent).toBe(`${base},${base + 60000}`);
  fireEvent.click(screen.getByRole("button", { name: "已存1分档案（固定）" }));
  expect(screen.queryByLabelText("live")).toBeNull();
  expect(screen.getByLabelText("archive").textContent).toBe(versions[0].id);
  fireEvent.change(screen.getByLabelText("对照的1分档案"), { target: { value: versions[1].id } });
  await screen.findByText(/标签间隔 2 分钟/);
  expect(screen.getByLabelText("archive").textContent).toBe(versions[1].id);
  view.unmount();
  view = render(f.page());
  expect(screen.queryByLabelText("live")).toBeNull();
  await waitFor(() => expect(screen.getByLabelText("archive").textContent).toBe(versions[1].id));
  fireEvent.click(screen.getByRole("button", { name: "本次观测（临时）" }));
  expect(screen.getByLabelText("live").textContent).toBe(`${base},${base + 60000}`);
  expect(f.trade).not.toHaveBeenCalled();
});
test("old instances and malformed snapshots are never drawn; restart resets segment and source selection", async () => {
  const f = fixture();
  const view = render(f.page());
  await screen.findByText(/标签范围可能重叠/);
  fireEvent.click(screen.getByRole("button", { name: "已存1分档案（固定）" }));
  const next = { ...market, instance_id: "replay-2" };
  view.rerender(f.page(next, series));
  expect(screen.queryByLabelText("archive")).toBeNull();
  expect(screen.queryByLabelText("live")).toBeNull();
  expect(screen.getByText(/旧实例响应不显示/)).toBeTruthy();
  view.rerender(f.page(next, { ...series, instance_id: next.instance_id }));
  expect(screen.getByLabelText("live").textContent).toBe(String(base + 240000));
  view.rerender(
    f.page(next, {
      ...series,
      instance_id: next.instance_id,
      bars: [series.bars[0], series.bars[0]],
    }),
  );
  expect(screen.queryByLabelText("live")).toBeNull();
  expect(screen.getByRole("alert").textContent).toContain("重复或乱序");
  expect(f.trade).not.toHaveBeenCalled();
});
