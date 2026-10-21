import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import type { Snapshot, IntradaySeries } from "../src/bridge/client";

test("isolated one-minute replay keeps archive and observed segments explicit", async ({
  page,
}, testInfo) => {
  test.setTimeout(90000);
  const errors: string[] = [];
  page.on("pageerror", e => errors.push(e.message));
  const base = Date.UTC(2026, 9, 13, 1),
    contract = "SHFE/rb/2026-10";
  const ns = (ms: number) => String(BigInt(ms) * 1000000n);
  const versions = ["a", "b"].map(c => ({
    id: c.repeat(64),
    revision: c.repeat(64),
    contract_id: contract,
    source: "tushare.ft_mins",
    interval_minutes: 1,
    rows: 120,
    begin: "2026-10-12",
    end: "2026-10-13",
  }));
  let instance = "replay-1",
    stale = false,
    reads = 0;
  const calls: string[] = [];
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    calls.push(request.method);
    // The repository harness starts isolated local services. Preserve only
    // their explicit startup checks; all market/data replies below are replayed.
    if (
      [
        "node.local",
        "market.local",
        "native.plugins.inspect",
        "node.data_tasks.local.open",
      ].includes(request.method)
    )
      return route.continue();
    expect([
      "runtime.snapshot",
      "data.datasets",
      "data.minutes.page",
      "data.tables.coverage",
      "market.minutes",
    ]).toContain(request.method);
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    const original = data.result.market!;
    const quote = {
      last: "101",
      bid: "100",
      ask: "102",
      previous_settlement: "100",
      previous_close: "100",
      open_interest_change: null,
      open: "100",
      upper_limit: null,
      lower_limit: null,
      high: "103",
      low: "99",
      open_interest: "50",
      bid_levels: [],
      ask_levels: [],
      bid_quantity: 1,
      ask_quantity: 1,
      volume: 80,
      source_ms: Date.now(),
      received_ms: Date.now(),
      trading_day: "20261013",
      action_day: "20261013",
      update_time: "09:04:01",
    };
    data.result.market = {
      ...original,
      service: original.service,
      instance_id: instance,
      transport_online: true,
      phase: "connected",
      error_code: 0,
      watchlist: [{ venue: "SHFE", symbol: "rb2610" }],
      subscriptions: [
        { venue: "SHFE", symbol: "rb2610", state: "subscribed", error_code: 0, quote },
      ],
      catalog: {
        ...original.catalog,
        contracts: [
          {
            venue: "SHFE",
            symbol: "rb2610",
            contract_id: contract,
            product: "rb",
            expiry: "20261015",
            multiplier: 10,
            price_tick: "1",
            name: "螺纹钢2610",
          },
        ],
      },
    };
    data.result.data = { ...data.result.data!, online: true, connection_id: "isolated-data" };
    if (request.method === "data.datasets") data.result.history_datasets = versions;
    if (request.method === "data.tables.coverage")
      data.result.table_coverage = { snapshot: "", sources: [], rows: [] };
    if (request.method === "market.minutes") {
      reads++;
      const intraday: IntradaySeries = {
        service: original.service,
        instance_id: stale ? "replay-1" : instance,
        venue: "SHFE",
        symbol: "rb2610",
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
      data.result.intraday = intraday;
    }
    if (request.method === "data.minutes.page") {
      const p = request.params;
      expect(versions.map(v => v.id)).toContain(p.id);
      const last = p.id === versions[0].id ? base + 60000 : base - 120000;
      const begin = last - 119 * 60000;
      data.result.history_page = {
        id: p.id,
        source: "tushare.ft_mins",
        contract_id: contract,
        interval_minutes: 1,
        manifest_sha256: p.id,
        total_rows: 120,
        matched_rows: 120,
        offset: p.offset,
        limit: p.limit,
        begin_ns: ns(begin),
        end_ns: ns(last),
        first_ns: ns(begin),
        last_ns: ns(last),
        bars: Array.from({ length: Math.min(p.limit, 120 - p.offset) }, (_, i) => ({
          timestamp_ns: ns(begin + (p.offset + i) * 60000),
          open: "100",
          high: "103",
          low: "99",
          close: String(100 + (i % 2)),
          volume: "10",
          amount: "1000",
          open_interest: "50",
        })),
      };
    }
    await route.fulfill({ response, json: data });
  });
  try {
    await page.goto("/");
    await openView(page, "market");
    await page.getByRole("button", { name: "rb2610", exact: true }).click();
    const pane = page.locator(".quote-pane").first();
    await pane.getByRole("button", { name: "1分", exact: true }).click();
    const segments = pane.getByRole("region", { name: "1分钟分段浏览" });
    await expect(segments).toContainText("标签范围可能重叠");
    await expect(segments).toContainText("本段仅画 1 根已观测分钟");
    await expect(segments).toContainText("未观测标签窗 2 个");
    await segments.getByLabel("保留观测段").selectOption(String(base));
    await expect(segments).toContainText("本段仅画 2 根已观测分钟");
    const before = reads;
    await expect.poll(() => reads).toBeGreaterThan(before);
    await expect(segments).toContainText("本段仅画 2 根已观测分钟");
    await segments.getByRole("button", { name: "已存1分档案（固定）" }).click();
    await expect(segments.locator(".market-history-pages")).toBeVisible();
    await expect(segments).not.toContainText("本段仅画");
    await segments.getByLabel("对照的1分档案").selectOption(versions[1].id);
    await expect(segments).toContainText("标签间隔 2 分钟");
    await expect(segments).toContainText("固定分钟档案版本");
    await openView(page, "backtest");
    await openView(page, "market");
    await expect(segments.getByRole("button", { name: "已存1分档案（固定）" })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    await expect(segments.getByLabel("对照的1分档案")).toHaveValue(versions[1].id);
    await segments.getByRole("button", { name: "本次观测（临时）" }).click();
    await expect(segments).toContainText("本段仅画 2 根已观测分钟");
    await segments.getByRole("button", { name: "已存1分档案（固定）" }).click();
    stale = true;
    instance = "replay-2";
    await expect(segments.getByRole("button", { name: "本次观测（临时）" })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    await expect(segments).not.toContainText("本段仅画");
    stale = false;
    await expect(segments).toContainText("本段仅画 1 根已观测分钟", { timeout: 15000 });
    await segments.getByText("分钟边界与来源", { exact: true }).click();
    await expect(segments).toContainText("replay-2");
    await segments.getByText("分钟边界与来源", { exact: true }).click();
    const plot = segments.getByRole("img", { name: "分钟 K 线", exact: true });
    await plot.scrollIntoViewIfNeeded();
    expect((await plot.boundingBox())!.height).toBeGreaterThan(100);
    await page.screenshot({ path: testInfo.outputPath("minute-segments.png"), fullPage: true });
    expect(errors).toEqual([]);
    expect(calls.filter(m => /connect|subscribe|trade|order|download|refresh/.test(m))).toEqual([]);
    await testInfo.attach("isolated-replay-evidence", {
      body: JSON.stringify(
        {
          mode: "intercepted-isolated-replay",
          provider_requests: 0,
          writes: 0,
          instance,
          reads,
          calls: [...new Set(calls)],
          bars: [0, 1, 4],
          gap_windows: 2,
          selected_version: versions[1].id,
          page_errors: errors,
        },
        null,
        2,
      ),
      contentType: "application/json",
    });
  } finally {
    if (!page.isClosed()) await page.unrouteAll({ behavior: "wait" });
  }
});
