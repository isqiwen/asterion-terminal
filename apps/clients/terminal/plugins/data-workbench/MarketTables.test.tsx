import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { Snapshot, TableCoverage, TaskRecord } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { BackendError, type TerminalContext } from "../contract";
import { MarketTables } from "./MarketTables";
import { plugin } from "./plugin";

registerTerminalPlugins([plugin]);

const provider = "asterion.data.tushare";
// One data source with a saved credential, offering the daily table of two exchanges.
function ready(change: (snapshot: Snapshot) => void = () => {}): Snapshot {
  const snapshot = readySnapshot();
  snapshot.data!.sources = [
    {
      id: "tushare.fut_daily",
      name: "Tushare",
      plugin_id: provider,
      normalization: "tushare.daily.v2",
      timezone: "Asia/Shanghai",
      timestamp_semantics: "trading_day",
      venues: ["SHFE", "DCE"],
      intervals: [0],
      max_requests_per_minute: 500,
      credential_required: true,
      connection: {
        credential_label_en: "Tushare Token",
        credential_label_zh: "Tushare Token",
        credential_required: true,
        credential_max_length: 256,
        remember_allowed: true,
        requests_per_minute_default: 60,
        requests_per_minute_max: 500,
      },
    },
  ];
  snapshot.data_credentials = [
    { provider, requests_per_minute: 120, remember: true, credential_ready: true },
  ];
  change(snapshot);
  return snapshot;
}
const update = (id: string, sequence: number): TaskRecord => ({
  kind: "table_update",
  id,
  state: "succeeded",
  attempt: 1,
  completed: 0,
  total: 100,
  error: "",
  result_digest: "",
  trading_day: "",
  instrument: "futures.daily",
  source_name: provider,
  submission_sequence: sequence,
  submitted_at_ms: 1,
  updated_at_ms: 2,
});
// SHFE holds three days with one trading day missing between them; DCE holds nothing.
const coverage: TableCoverage = {
  snapshot: "c".repeat(64),
  sources: [
    {
      table: "futures.daily",
      schema: 1,
      source: "tushare.fut_daily.v1",
      plugin_id: provider,
      venues: ["SHFE", "DCE"],
    },
  ],
  rows: [
    {
      table: "futures.daily",
      schema: 1,
      source: "tushare.fut_daily.v1",
      exchange: "SHFE",
      partitions: 3,
      rows: 690,
      first_period: "2026-09-29",
      last_period: "2026-10-09",
      acquired_at_ns: "1791717000000000000",
      missing_days: 1,
      missing_examples: ["2026-09-30"],
    },
  ],
};
const page = (
  snapshot: Snapshot,
  trade: TerminalContext["trade"] = async () => {},
  query: TerminalContext["query"] = async () => ({ ...snapshot, table_coverage: coverage }),
) => (
  <MarketTables
    snapshot={snapshot}
    busy={false}
    error=""
    navigate={() => {}}
    openSettings={() => {}}
    query={query}
    trade={trade}
  />
);

test("what is stored, what is missing inside it and what was never downloaded are told apart", async () => {
  render(page(ready()));
  const table = await screen.findByRole("table", { name: "全市场数据覆盖" });
  const [shfe, dce] = within(table).getAllByRole("row").slice(1);
  expect(shfe.textContent).toContain("2026-09-29 — 2026-10-09");
  expect(shfe.textContent).toContain("1 个：2026-09-30");
  expect(dce.textContent).toContain("尚未下载");
  // One exchange holds data, so an update may continue without a start date.
  expect((screen.getByLabelText("起始日期") as HTMLInputElement).required).toBe(false);
});

test("with nothing stored the first download needs a start date", async () => {
  const snapshot = ready();
  render(
    page(snapshot, undefined, async () => ({
      ...snapshot,
      table_coverage: { ...coverage, rows: [] },
    })),
  );
  await screen.findByRole("table", { name: "全市场数据覆盖" });
  expect((screen.getByLabelText("起始日期") as HTMLInputElement).required).toBe(true);
});

test("an update started before the page was opened is still shown", async () => {
  const snapshot = ready(state => {
    state.task_service!.tasks = [
      {
        ...update("tables-earlier", 3),
        state: "failed",
        error: "Tushare reported an error: fut_daily",
      },
      { ...update("tables-running", 7), state: "running", completed: 43 },
    ];
  });
  render(page(snapshot));
  expect((await screen.findByRole("status")).textContent).toBe("正在更新：43%");
});

test("an update keeps its identity until it is accepted, then reports partition by partition", async () => {
  const submitted: Record<string, unknown>[] = [];
  let accept = false;
  const trade = vi.fn(async (method: string, params?: Record<string, unknown>) => {
    if (method !== "data.tables.update.submit") return;
    submitted.push(params!);
    if (!accept) throw new BackendError("unavailable", "task service is not connected");
  });
  const view = render(page(ready(), trade));
  await screen.findByRole("table", { name: "全市场数据覆盖" });
  fireEvent.change(screen.getByLabelText("起始日期"), { target: { value: "2026-09-01" } });
  fireEvent.click(screen.getByRole("button", { name: "开始更新" }));
  await screen.findByRole("alert");
  accept = true;
  fireEvent.click(screen.getByRole("button", { name: "开始更新" }));
  await waitFor(() => expect(submitted).toHaveLength(2));
  expect(submitted[1]).toEqual(submitted[0]);
  expect(submitted[0]).toMatchObject({
    provider,
    tables: ["futures.daily"],
    begin_day: "2026-09-01",
    end_day: "",
    requests_per_minute: 120,
    token: "",
  });
  const id = submitted[0].id as string;
  const task: TaskRecord = { ...update(id, 1), completed: 100, result_digest: "d".repeat(64) };
  const finished = ready(snapshot => {
    snapshot.task_service!.tasks = [task];
    snapshot.task_result = {
      id,
      kind: "table_update",
      task,
      experiment: {
        provider,
        tables: ["futures.daily"],
        begin_day: "2026-09-01",
        end_day: "",
        requests_per_minute: 120,
      },
      result: {
        stored: 41,
        unchanged: 12,
        rejected: [
          {
            table: "futures.daily",
            exchange: "DCE",
            period: "2026-09-18",
            reason:
              'futures.daily DCE 2026-09-18: row 7 "DCE/i/2027-01" column amount: "1.5e3" is not a plain decimal number',
          },
        ],
        unpublished: 2,
        unpublished_examples: [{ table: "futures.daily", exchange: "SHFE", period: "2026-10-09" }],
        waiting: ["futures.receipts SHFE", "futures.rankings SHFE"],
        snapshot: "e".repeat(64),
      },
    };
  });
  view.rerender(page(finished, trade));
  expect((await screen.findAllByRole("status")).map(item => item.textContent)).toContain(
    "更新完成：新入库 41 个分区，12 个没有变化。",
  );
  expect(trade).toHaveBeenCalledWith("task.result", { id });
  const rejected = screen.getByRole("table", { name: "未能入库的分区" });
  expect(rejected.textContent).toContain("日线 DCE 2026-09-18");
  expect(rejected.textContent).toContain('column amount: "1.5e3" is not a plain decimal number');
  expect(document.body.textContent).toContain(
    "数据源对 2 个分区没有返回数据，例如 日线 SHFE 2026-10-09。",
  );
  expect(document.body.textContent).toContain(
    "有 2 组数据表和交易所还没有数据，需要给出起始日期才会下载，例如 仓单日报 SHFE、成交持仓排名 SHFE。",
  );
});

test("a column is named for what it means in its table, with what a reader must know first", async () => {
  const snapshot = ready();
  const receipts = {
    table: "futures.receipts",
    schema: 1,
    source: "tushare.fut_wsr.v4",
    plugin_id: provider,
    venues: ["SHFE"],
  };
  const held: TableCoverage = {
    snapshot: "c".repeat(64),
    sources: [...coverage.sources, receipts],
    rows: [
      ...coverage.rows,
      {
        ...coverage.rows[0],
        table: receipts.table,
        source: receipts.source,
        partitions: 1,
        rows: 2,
        first_period: "2026-10-09",
        missing_days: 0,
        missing_examples: [],
      },
    ],
  };
  const asked: Record<string, unknown>[] = [];
  const query: TerminalContext["query"] = async (method, params) => {
    if (method !== "data.tables.page") return { ...snapshot, table_coverage: held };
    asked.push(params!);
    return {
      ...snapshot,
      table_page: {
        revision: {
          sequence: 9,
          table: receipts.table,
          schema: 1,
          source: receipts.source,
          exchange: "SHFE",
          period: "2026-10-09",
          content: "a".repeat(64),
          rows: 2,
          bytes: 1024,
          acquired_at_ns: "1791717000000000000",
          provider_artifact: "b".repeat(64),
          snapshot: "c".repeat(64),
        },
        columns: ["product", "line", "warehouse", "volume", "unit", "later_column"],
        rows: [
          ["cu", "1", "上海国储天威", "8285", "吨", "x"],
          ["cu", "2", "中储大场", null, "吨", null],
        ],
        offset: 0,
      },
    };
  };
  render(page(snapshot, undefined, query));
  const table = await screen.findByRole("table", { name: "全市场数据覆盖" });
  const row = within(table)
    .getAllByRole("row")
    .find(item => item.textContent?.startsWith("仓单日报"))!;
  fireEvent.click(within(row).getByRole("button", { name: "查看数据" }));
  const viewer = await screen.findByRole("table", { name: "分区数据" });
  const headers = within(viewer).getAllByRole("columnheader");
  // The daily table calls its volume 成交量; here it is receipts.
  expect(headers.map(item => item.textContent)).toEqual([
    "品种",
    "行",
    "仓库",
    "今日仓单量",
    "单位",
    "later_column",
  ]);
  expect(headers.map(item => item.className)).toEqual(["", "numeric", "", "numeric", "", ""]);
  expect(within(viewer).getAllByRole("row")[2].textContent).toBe("cu2中储大场—吨—");
  expect(asked[0]).toMatchObject({ table: receipts.table, source: receipts.source });
  expect(screen.getByRole("region", { name: "分区数据" }).textContent).toContain(
    "行号是当天报表里该品种的第几行，不同日期之间不对应。",
  );
});
