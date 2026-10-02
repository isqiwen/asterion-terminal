import { test, expect } from "./test";
import type { Snapshot, DailyPage } from "../src/bridge/client";

test("daily source viewer and market chart preserve dates, missing settlement and paging", async ({
  page,
}) => {
  const calls: { start: string; end: string; offset: number; period?: string }[] = [];
  let mismatch = false;
  let connection = "first-connection";
  const date = (i: number) => new Date(Date.UTC(2023, 0, 1 + i)).toISOString().slice(0, 10);
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!["runtime.snapshot", "research.daily.page"].includes(request.method))
      return route.continue();
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    data.result.research = {
      ...data.result.research!,
      connection_id: connection,
      service: "daily-fixture",
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
    data.result.daily_page = null;
    if (request.method === "research.daily.page") {
      expect(request.params.id).toBe("daily-fixture");
      calls.push(request.params);
      const filtered = !!request.params.start;
      const period = request.params.period;
      expect(["day", "week", "month", "quarter", "year"]).toContain(period);
      const total =
        period === "week"
          ? 18
          : period === "month"
            ? 4
            : period === "quarter"
              ? 2
              : period === "year"
                ? 1
                : 120;
      const count = filtered ? 0 : total;
      const offset = request.params.offset;
      data.result.daily_page = {
        id: mismatch ? "wrong-dataset" : "daily-fixture",
        source: "tushare.fut_daily",
        contract_id: "SHFE/rb/2026-10",
        manifest_sha256: "b".repeat(64),
        total_rows: total,
        matched_rows: count,
        offset,
        limit: request.params.limit,
        first_day: date(0),
        last_day: date(119),
        begin_day: request.params.start || date(0),
        end_day: request.params.end || date(119),
        bars: Array.from(
          { length: Math.max(0, Math.min(request.params.limit, count - offset)) },
          (_, i) => ({
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
            macd:
              offset + i >= 100
                ? { diff: 0, signal: 0, histogram: 0 }
                : offset + i >= 33
                  ? { diff: 1.2, signal: 0.7, histogram: 1 }
                  : undefined,
          }),
        ),
        period,
      } satisfies DailyPage;
      mismatch = false;
    }
    await route.fulfill({ response, json: data });
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
  connection = "viewer-new-connection";
  await expect(viewer).toHaveCount(0);
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
  await expect(table.getByRole("row").nth(1)).toContainText("2023-01-01");
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
  const previousCalls = calls.length;
  connection = "same-named-service-new-connection";
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
  await nav.getByRole("button", { name: "数据", exact: true }).click();
  await expect(viewer).toHaveCount(0);
  await expect(page.getByRole("button", { name: "查看数据", exact: true })).toBeVisible();
  await nav.getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await expect(page.locator(".history-market-contracts")).toContainText("日K");
  await expect(page.locator(".history-market-board").getByRole("img")).toBeVisible();
});
