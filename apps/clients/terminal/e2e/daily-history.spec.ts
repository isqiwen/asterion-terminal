import { openView } from "./navigation-helper";
import { test, expect, type Page } from "./test";
import { writeFile } from "node:fs/promises";
import type { Snapshot, TableDailyPage } from "../src/bridge/client";

const date = (i: number) => new Date(Date.UTC(2023, 0, 1 + i)).toISOString().slice(0, 10);
// A data service with the market-wide daily table under one snapshot;
// `answer` fills in the reply
// to a page request. The connection can be changed while the page is open.
async function fixture(
  page: Page,
  answer: (method: string, params: Record<string, unknown>, result: Snapshot) => void,
) {
  const state = { connection: "first-connection" };
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (
      !["runtime.snapshot", "data.datasets", "data.tables.coverage", "data.tables.daily"].includes(
        request.method,
      )
    )
      return route.continue();
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (!data.result.data || !data.result.data) return route.fulfill({ response, json: data });
    data.result.data.connection_id = state.connection;
    data.result.data = {
      ...data.result.data!,
      connection_id: state.connection,
      online: true,
      remote: false,
      tasks: [],
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
    data.result.history_datasets = [];
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

test("the daily chart reads the contract from the daily table, by period and page", async ({
  page,
}, info) => {
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
  await openView(page, "watchlist");
  const chart = page.locator(".watchlist-chart-pane").nth(0);
  await expect(chart.getByRole("button", { name: "日K", exact: true })).toBeEnabled();
  await chart.getByRole("button", { name: "日K", exact: true }).click();
  await expect(chart.getByRole("button", { name: "日K", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(chart.getByRole("img")).toBeVisible();
  await expect(chart.locator(".market-history-pages")).toContainText("1–100 / 120");
  await expect(chart.getByRole("img").locator("canvas").first()).toBeVisible();
  const plot = chart.getByRole("img");
  for (let step = 0; step < 6; step++) await plot.press("+");
  await plot.press("Home");
  await expect(chart.locator("[data-oscillator-empty]")).toHaveText("当前区间无 MACD 数据");
  await plot.press("End");
  await expect(chart.locator("[data-oscillator-empty]")).toHaveCount(0);
  await expect(chart.getByRole("img").locator("canvas").first()).toBeVisible();
  await chart.getByRole("button", { name: "向后", exact: true }).click();
  await expect(chart.locator(".market-history-pages")).toContainText("101–120 / 120");
  await expect(chart.locator("[data-oscillator-empty]")).toHaveCount(0);
  await expect(chart.getByRole("img").locator("canvas").first()).toBeVisible();
  await expect(chart.locator("[data-oscillator-legend]")).toContainText("DIFF: 0.00");
  // What the chart asked for: the contract, under the snapshot the coverage named.
  expect(calls.at(-1)).toMatchObject({
    contract: "SHFE/rb/2026-10",
    period: "day",
    offset: 41,
    daily: { snapshot: "c".repeat(64), daily_source: "tushare.fut_daily.v3" },
  });
  // A real workbench navigation unmounts this chart. Returning from research
  // must retain the source-bound data page, including a second round trip.
  for (let i = 0; i < 2; i++) {
    const beforeReturn = calls.length;
    await openView(page, "backtest");
    await expect(page.locator(".watchlist-chart-pane")).toHaveCount(0);
    await openView(page, "watchlist");
    await expect(chart.getByRole("button", { name: "日K", exact: true })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    await expect(chart.locator(".market-history-pages")).toContainText("101–120 / 120");
    expect(
      calls.slice(beforeReturn).some(call => call.period === "day" && call.offset === 41),
    ).toBe(true);
  }
  await chart.scrollIntoViewIfNeeded();
  await page.screenshot({ path: info.outputPath("history-resume.png"), fullPage: true });
  await writeFile(
    info.outputPath("history-resume.json"),
    JSON.stringify(
      {
        restored_page: "101–120 / 120",
        round_trips: 2,
        restored_upper_pane_request: calls
          .filter(call => call.period === "day" && call.offset === 41)
          .at(-1),
        calls_before_service_change: calls.length,
      },
      null,
      2,
    ),
  );
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
    if (period === "year") {
      await expect(chart.getByRole("img")).toHaveAttribute("data-visible-count", "1");
      await chart.getByRole("img").press("ArrowLeft");
      await expect(chart.locator(".price-chart-cursor")).not.toBeEmpty();
    }
    expect(calls.at(-1)?.period).toBe(period);
    expect(calls.at(-1)?.offset).toBe(0);
  }
  await expect(chart.getByRole("button", { name: "1 min", exact: true })).toBeDisabled();
});
