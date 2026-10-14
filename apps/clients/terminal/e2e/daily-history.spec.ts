import { test, expect, type Page } from "./test";
import type { Snapshot, DailyPage, TableDailyPage } from "../src/bridge/client";

const date = (i: number) => new Date(Date.UTC(2023, 0, 1 + i)).toISOString().slice(0, 10);
// A data and task service with one daily download of rb2610 and, beside it,
// the market-wide daily table under one snapshot; `answer` fills in the reply
// to a page request. The connection can be changed while the page is open.
async function fixture(
  page: Page,
  answer: (method: string, params: Record<string, unknown>, result: Snapshot) => void,
) {
  const state = { connection: "first-connection" };
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (
      ![
        "runtime.snapshot",
        "data.daily.page",
        "data.datasets",
        "data.tables.coverage",
        "data.tables.daily",
      ].includes(request.method)
    )
      return route.continue();
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (!data.result.task_service || !data.result.data)
      return route.fulfill({ response, json: data });
    data.result.data.connection_id = state.connection;
    data.result.task_service = {
      ...data.result.task_service!,
      connection_id: state.connection,
      online: true,
      remote: false,
      tasks: [
        {
          id: "daily-fixture",
          kind: "daily_download",
          data_source: "tushare.fut_daily",
          state: "succeeded",
          attempt: 1,
          instrument: "SHFE/rb/2026-10",
          submission_sequence: 1,
          source_name: "Explicit daily fixture",
          result_digest: "b".repeat(64),
          history_dataset_id: "daily-fixture",
          completed: 1,
          total: 1,
          error: "",
          trading_day: "",
          submitted_at_ms: 1,
          updated_at_ms: 1,
        },
      ],
    };
    data.result.market = {
      ...data.result.market!,
      catalog: {
        phase: "ready",
        error_code: "",
        diagnostic: "",
        trading_day: "2026-09-25",
        contracts: [
          {
            venue: "SHFE",
            symbol: "rb2610",
            product: "rb",
            expiry: "20261015",
            contract_id: "SHFE/rb/2026-10",
            multiplier: 10,
            price_tick: "1",
            name: "螺纹钢2610",
          },
        ],
      },
      transport_online: true,
      phase: "connected",
      watchlist: [{ venue: "SHFE", symbol: "rb2610" }],
      subscriptions: [
        { venue: "SHFE", symbol: "rb2610", state: "subscribed", error_code: 0, quote: null },
      ],
    };
    data.result.history_datasets = [
      {
        id: "daily-fixture",
        contract_id: "SHFE/rb/2026-10",
        source: "tushare.fut_daily",
        revision: "b".repeat(64),
        begin: date(0),
        end: date(119),
        interval_minutes: 0,
        rows: 120,
      },
    ];
    data.result.daily_page = null;
    data.result.table_coverage = {
      snapshot: "c".repeat(64),
      sources: (
        [
          ["futures.contracts", "tushare.fut_basic.v1"],
          ["futures.calendar", "tushare.trade_cal.v1"],
          ["futures.daily", "tushare.fut_daily.v3"],
        ] as const
      ).map(([table, source]) => ({
        table,
        schema: 1,
        source,
        plugin_id: "asterion.data.tushare",
        venues: ["SHFE"],
      })),
      rows: [
        {
          table: "futures.daily",
          schema: 1,
          source: "tushare.fut_daily.v3",
          exchange: "SHFE",
          partitions: 120,
          rows: 120,
          first_period: date(0),
          last_period: date(119),
          acquired_at_ns: "1791717000000000000",
          missing_days: 0,
          missing_examples: [],
        },
      ],
    };
    answer(request.method, request.params, data.result);
    await route.fulfill({ response, json: data });
  });
  return state;
}

test("daily source viewer preserves dates, missing settlement and paging", async ({ page }) => {
  const calls: { start: string; end: string; offset: number }[] = [];
  let mismatch = false;
  const state = await fixture(page, (method, params, result) => {
    if (method !== "data.daily.page") return;
    expect(params.id).toBe("daily-fixture");
    expect(params).not.toHaveProperty("period");
    calls.push(params as (typeof calls)[number]);
    const count = params.start ? 0 : 120;
    const offset = params.offset as number;
    const limit = params.limit as number;
    result.daily_page = {
      id: mismatch ? "wrong-dataset" : "daily-fixture",
      source: "tushare.fut_daily",
      contract_id: "SHFE/rb/2026-10",
      manifest_sha256: "b".repeat(64),
      total_rows: 120,
      matched_rows: count,
      offset,
      limit,
      first_day: date(0),
      last_day: date(119),
      begin_day: (params.start as string) || date(0),
      end_day: (params.end as string) || date(119),
      bars: Array.from({ length: Math.max(0, Math.min(limit, count - offset)) }, (_, i) => ({
        trading_day: date(offset + i),
        open: "100.00000001",
        high: "102",
        low: "99",
        close: "101.5",
        volume: "20",
        amount: "12345678.12345678",
        open_interest: "1000",
        previous_close: null,
        previous_settlement: "99.5",
        settlement: null,
      })),
    } satisfies DailyPage;
    mismatch = false;
  });
  await page.goto("/");
  const nav = page.getByRole("navigation", { name: "业务工作区" });
  await nav.getByRole("button", { name: "数据", exact: true }).click();
  await page
    .getByRole("combobox", { name: "数据源", exact: true })
    .selectOption("tushare.fut_daily");
  await expect(page.getByRole("combobox", { name: "数据周期", exact: true })).toHaveValue("day");
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
  const viewer = page.getByRole("region", { name: "历史数据查看", exact: true });
  await expect(viewer.getByRole("img")).toBeVisible();
  const table = viewer.getByRole("region", { name: "日线数据表" });
  await expect(table.getByRole("columnheader", { name: "交易日期", exact: true })).toBeVisible();
  await expect(table.getByRole("row").nth(1)).toContainText("2023-01-01");
  await expect(table.getByRole("row").nth(1)).toContainText("100.00000001");
  await expect(table.getByRole("row").nth(1).getByRole("cell").nth(8)).toHaveText("—");
  await expect(table.getByRole("row").nth(1).getByRole("cell").nth(9)).toHaveText("99.5");
  await expect(table.getByRole("row").nth(1).getByRole("cell").nth(10)).toHaveText("—");
  await viewer.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(table.getByRole("row").nth(1)).toContainText(date(100));
  await viewer.getByLabel("开始交易日期", { exact: true }).fill("2023-05-01");
  await viewer.getByLabel("结束交易日期", { exact: true }).fill("2023-05-02");
  await viewer.getByRole("button", { name: "查看区间", exact: true }).click();
  await expect(viewer).toContainText("此范围没有记录");
  expect(calls.at(-1)?.start).toBe("2023-05-01");
  expect(calls.at(-1)?.end).toBe("2023-05-02");
  await viewer.getByRole("button", { name: "全部时间", exact: true }).click();
  await expect(table.getByRole("row").nth(1)).toContainText("2023-01-01");
  mismatch = true;
  await viewer.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(viewer.getByRole("alert")).toContainText("返回的数据与所选数据集不匹配");
  await expect(table.getByRole("row").nth(1)).toContainText("2023-01-01");
  state.connection = "viewer-new-connection";
  await expect(viewer).toHaveCount(0);
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
  await expect(table.getByRole("row").nth(1)).toContainText("2023-01-01");
});

test("the daily chart reads the contract from the daily table, by period and page", async ({
  page,
}) => {
  const calls: { contract: string; period: string; offset: number }[] = [];
  const state = await fixture(page, (method, params, result) => {
    if (method !== "data.tables.daily") return;
    calls.push(params as (typeof calls)[number]);
    const period = params.period as TableDailyPage["period"];
    const total = { day: 120, week: 18, month: 4, quarter: 2, year: 1 }[period];
    const offset = params.offset as number;
    const limit = params.limit as number;
    result.table_daily = {
      daily: params.daily as TableDailyPage["daily"],
      contract: params.contract as string,
      period,
      total,
      offset,
      limit,
      candles: Array.from({ length: Math.max(0, Math.min(limit, total - offset)) }, (_, i) => ({
        trading_day: date(offset + i),
        open: "100.00000001",
        high: "102",
        low: "99",
        close: "101.5",
        volume: "20",
        macd:
          offset + i >= 100
            ? { diff: 0, signal: 0, histogram: 0 }
            : offset + i >= 33
              ? { diff: 1.2, signal: 0.7, histogram: 1 }
              : undefined,
      })),
    } satisfies TableDailyPage;
  });
  await page.goto("/");
  const nav = page.getByRole("navigation", { name: "业务工作区" });
  await nav.getByRole("button", { name: "自选", exact: true }).click();
  const chart = page.locator(".watchlist-chart-pane").nth(0);
  await expect(chart.getByRole("button", { name: "日K", exact: true })).toBeEnabled();
  await chart.getByRole("button", { name: "日K", exact: true }).click();
  await expect(chart.getByRole("button", { name: "日K", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(chart.getByRole("img")).toBeVisible();
  await expect(chart.locator(".market-history-pages")).toContainText("1–100 / 120");
  await expect(chart.locator("[data-oscillator-axis]")).toBeVisible();
  const plot = chart.getByRole("img");
  for (let step = 0; step < 6; step++) await plot.press("+");
  await plot.press("Home");
  await expect(chart.locator("[data-oscillator-empty]")).toHaveText("当前区间无 MACD 数据");
  await expect(chart.locator("[data-oscillator-axis]")).toHaveCount(0);
  await plot.press("End");
  await expect(chart.locator("[data-oscillator-empty]")).toHaveCount(0);
  await expect(chart.locator("[data-oscillator-axis]")).toBeVisible();
  await chart.getByRole("button", { name: "向后", exact: true }).click();
  await expect(chart.locator(".market-history-pages")).toContainText("101–120 / 120");
  await expect(chart.locator("[data-oscillator-empty]")).toHaveCount(0);
  await expect(chart.locator("[data-oscillator-axis]")).toBeVisible();
  await expect(chart.locator("[data-oscillator-legend]")).toContainText("DIFF: 0.00");
  // What the chart asked for: the contract, under the snapshot the coverage named.
  expect(calls.at(-1)).toMatchObject({
    contract: "SHFE/rb/2026-10",
    period: "day",
    offset: 41,
    daily: { snapshot: "c".repeat(64), daily_source: "tushare.fut_daily.v3" },
  });
  const previousCalls = calls.length;
  state.connection = "same-named-service-new-connection";
  await expect(chart.locator(".market-history-pages")).toContainText("1–100 / 120");
  await expect.poll(() => calls.length).toBeGreaterThan(previousCalls);
  for (const [label, period, total] of [
    ["周线", "week", 18],
    ["月线", "month", 4],
    ["季线", "quarter", 2],
    ["年线", "year", 1],
  ] as const) {
    await chart.getByRole("button", { name: label, exact: true }).click();
    await expect(chart.getByRole("button", { name: label, exact: true })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    await expect(chart.locator(".market-history-pages")).toContainText(`1–${total} / ${total}`);
    await expect(chart.locator("[data-oscillator-empty]")).toHaveText("当前区间无 MACD 数据");
    await expect(chart.locator("[data-oscillator-axis]")).toHaveCount(0);
    if (period === "year") {
      const alignment = await chart.getByRole("img").evaluate(svg => {
        const label = svg.querySelector<SVGTextElement>("[data-chart-time]")!;
        const crosshair = svg.querySelector<SVGPathElement>(".price-chart-crosshair")!;
        const bounds = label.getBBox();
        return Math.abs(bounds.x + bounds.width / 2 - crosshair.getPointAtLength(0).x);
      });
      expect(alignment).toBeLessThan(1);
    }
    expect(calls.at(-1)?.period).toBe(period);
    expect(calls.at(-1)?.offset).toBe(0);
  }
  await expect(chart.getByRole("button", { name: "1 min", exact: true })).toBeDisabled();
  // The history board opens the contract on its days as well.
  await nav.getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await expect(page.locator(".history-market-contracts")).toContainText("螺纹钢2610");
  await expect(page.locator(".history-market-board").getByRole("img")).toBeVisible();
});
