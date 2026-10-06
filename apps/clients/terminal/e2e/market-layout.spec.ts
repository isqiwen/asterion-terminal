import { test, expect } from "./test";
import type { Snapshot, HistoryPage, LiveMarket } from "../src/bridge/client";

test.afterEach(async ({ page }) => {
  await page.unrouteAll({ behavior: "wait" });
});

test("dense market board links real dataset commands, sorting, pagination and contract boundaries", async ({
  page,
}) => {
  test.setTimeout(90000);
  const pageErrors: string[] = [];
  page.on("pageerror", error => pageErrors.push(error.message));
  let historyPage: HistoryPage | null = null;
  let fail = false;
  let historicalOnly = false;
  let catalogNames = false;
  let marketOnline = true;
  let quoteHistoryAvailable = true;
  let quoteHistoryOffset = 0;
  let catalogLabel = "螺纹钢2601";
  const start = 1790582400000000000n;
  // Explicit visual fixture: never installed in the product or used as live data.
  const subscriptions: LiveMarket["subscriptions"] = [
    ["SHFE", "rb"],
    ["DCE", "i"],
    ["CZCE", "TA"],
    ["CFFEX", "IF"],
    ["INE", "sc"],
    ["GFEX", "si"],
  ].flatMap(([venue, product]) =>
    Array.from({ length: product === "IF" ? 4 : 8 }, (_, index) => ({
      venue,
      symbol: `${product}260${index + 1}`,
      state: "subscribed",
      error_code: 0,
      quote: {
        last: String(95 + index * 2),
        bid: String(94 + index * 2),
        ask: String(96 + index * 2),
        previous_settlement: "100",
        previous_close: "98.12500001",
        open_interest_change: null,
        open: index === 7 ? null : String(90 + index),
        upper_limit: null,
        lower_limit: null,
        high: "120",
        low: "90",
        open_interest: String(15000 + index * 200),
        bid_levels: Array.from({ length: 4 }, () => ({ price: null, quantity: null })),
        ask_levels: Array.from({ length: 4 }, () => ({ price: null, quantity: null })),
        bid_quantity: 12,
        ask_quantity: 8,
        volume: 5000 + index * 120,
        source_ms: Date.now(),
        received_ms: Date.now(),
        trading_day: "20260929",
        action_day: "20260929",
        update_time: "10:00:00",
      },
    })),
  );
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!["runtime.snapshot", "data.minutes.page", "data.datasets"].includes(request.method))
      return route.continue();
    if (request.method === "data.minutes.page") {
      expect(["linked-history-fixture", "history-15min"]).toContain(request.params.id);
      expect(request.params).not.toHaveProperty("directory");
      if (fail) {
        fail = false;
        return route.fulfill({ status: 503, body: "fixture read failure" });
      }
      expect(request.params.include_macd).toBe(true);
      const offset = request.params.offset;
      historyPage = {
        id: request.params.id,
        source: "tushare.ft_mins",
        contract_id: "SHFE/rb/2026-01",
        interval_minutes: request.params.id === "history-15min" ? 15 : 5,
        manifest_sha256: "b".repeat(64),
        total_rows: 120,
        matched_rows: 120,
        offset,
        limit: request.params.limit,
        begin_ns: String(start),
        end_ns: String(start + 119n * 300000000000n),
        first_ns: String(start),
        last_ns: String(start + 119n * 300000000000n),
        bars: Array.from({ length: Math.min(request.params.limit, 120 - offset) }, (_, index) => {
          const price = 100 + Math.sin((index + offset) / 7) * 8;
          return {
            timestamp_ns: String(start + BigInt(index + offset) * 300000000000n),
            ...(index + offset < 33
              ? {}
              : {
                  macd: {
                    diff: Math.sin((index + offset) / 7),
                    signal: Math.sin((index + offset - 3) / 7) * 0.8,
                    histogram: Math.sin((index + offset + 5) / 7) * 0.7,
                  },
                }),
            open: price.toFixed(2),
            high: (price + 2).toFixed(2),
            low: (price - 2).toFixed(2),
            close: (price + ((index + offset) % 3 === 0 ? -1 : 1)).toFixed(2),
            volume: String(20 + (index % 7)),
            amount: "1000",
            open_interest: "500",
          };
        }),
      };
    }
    const requestedPage = historyPage;
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (catalogNames)
      data.result.history_contracts = {
        source: "tushare.ft_mins",
        exchange: "SHFE",
        product: "RB",
        cutoff_ns: "0",
        items: [
          {
            code: "SHFE/rb/2026-01",
            name: catalogLabel,
            list_date: "20250101",
            delist_date: "20260115",
            multiplier: null,
            per_unit: "5.00000001",
            trade_unit: "吨",
            quote_unit: "元/吨",
          },
        ],
      };
    // The fixture represents a continuing feed while online. Reusing its
    // initial receipt time makes a slow layout run correctly look stale.
    if (marketOnline) {
      const observedAt = Date.now();
      for (const row of subscriptions) {
        if (!row.quote) continue;
        row.quote.source_ms = observedAt;
        row.quote.received_ms = observedAt;
      }
    }
    data.result.market = {
      remote: false,
      host: "localhost",
      port: 0,
      service: data.result.market!.service,
      instance_id: "fixture",
      phase: "connected",
      error_code: 0,
      transport_online: marketOnline,
      sequence: 1,
      out_of_order: 0,
      watchlist: subscriptions.map(({ venue, symbol }) => ({ venue, symbol })),
      catalog: {
        phase: "unconfigured",
        error_code: "",
        diagnostic: "",
        trading_day: "",
        contracts: subscriptions.map(row => ({
          venue: row.venue,
          symbol: row.symbol,
          contract_id: `${row.venue}/${row.symbol.replace(/[0-9]+$/, "").toLowerCase()}/20${row.symbol.slice(-4, -2)}-${row.symbol.slice(-2)}`,
          product: row.symbol.replace(/[0-9]+$/, ""),
          expiry: "20260930",
          multiplier: 10,
          price_tick: "1",
          name: "",
        })),
      },
      subscriptions,
      history: {
        stream_id: "fixture-stream",
        generation: "1",
        available: quoteHistoryAvailable,
        interrupted: false,
        points: Array.from({ length: 100 }, (_, relativeIndex) => {
          const index = relativeIndex + quoteHistoryOffset;
          return {
            venue: "SHFE",
            symbol: "rb2601",
            timestamp_ns: String(start + BigInt(index) * 1000000000n),
            price: (100 + Math.sin(index / 6) * 5).toFixed(2),
            ...(index > 0 ? { volume: String(10 + (index % 17) * 3) } : {}),
          };
        }),
      },
    };
    if (historicalOnly) {
      data.result.market = null;
      // Historical versions remain available when Task is offline and has no visible history.
      data.result.task_service = { ...data.result.task_service!, online: false, tasks: [] };
    }
    data.result.history_page = requestedPage;
    if (request.method === "data.datasets")
      data.result.history_datasets = [5, 15]
        .map(interval => ({
          id: interval === 5 ? "linked-history-fixture" : "history-15min",
          revision: "b".repeat(64),
          contract_id: "SHFE/rb/2026-01",
          source: "tushare.ft_mins",
          interval_minutes: interval,
          begin: String(start),
          end: String(start + 120n * 60000000000n),
          rows: 120,
        }))
        .filter(
          item => !request.params.contract_id || item.contract_id === request.params.contract_id,
        );
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  const assets = page.getByRole("navigation", { name: "资产类别" });
  const categories = page.getByRole("navigation", { name: "期货分类" });
  await expect(assets.getByRole("button")).toHaveCount(7);
  await expect(categories.getByRole("button")).toHaveCount(13);
  await assets.getByRole("button", { name: "外汇", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("该资产行情源尚未接入");
  await expect(page.locator(".market-exchange-groups")).toHaveCount(0);
  await assets.getByRole("button", { name: "期货", exact: true }).click();
  await categories.getByRole("button", { name: "商品指数", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("该指数行情源尚未接入");
  await categories.getByRole("button", { name: "上期所", exact: true }).click();
  await expect(page.locator(".market-exchange-groups tbody tr")).toHaveCount(8);
  await categories.getByRole("button", { name: "主力合约", exact: true }).click();
  await expect(page.locator(".market-exchange-groups tbody tr")).toHaveCount(6);
  await categories.getByRole("button", { name: "期货全景", exact: true }).click();
  await expect(page.locator(".market-exchange-groups tbody tr")).toHaveCount(6);
  const groups = page.locator(".market-exchange-groups");
  // Product overview rows use the reference "主连" label for the dated
  // contract with the largest open interest; the code stays the accessible name.
  await expect(groups).toContainText("螺纹钢主连");
  await expect(groups.getByRole("button", { name: "rb2608", exact: true })).toHaveCount(1);
  await expect(groups.getByRole("button", { name: "rb2601", exact: true })).toHaveCount(0);
  await page.getByRole("searchbox").fill("rb2601");
  await expect(page.locator(".market-exchange-groups tbody tr")).toHaveCount(1);
  await expect(page.locator(".market-exchange-groups")).toContainText("rb2601");
  await page.getByRole("searchbox").fill("");
  const menu = page.getByRole("dialog", { name: "行情设置" });
  await page.getByRole("button", { name: "行情设置" }).click();
  await menu.getByRole("button", { name: "全部月份", exact: true }).click();
  await page.keyboard.press("Escape");
  await expect(page.locator(".market-exchange-groups tbody tr")).toHaveCount(44);
  const shfe = page.getByRole("region", { name: "上海商品期货" });
  await expect(shfe.locator("tbody tr").first()).toContainText("rb2608");
  await expect(shfe.locator("tbody tr").first()).toContainText("1.64万");
  await expect(shfe.locator('td[data-tone="up"]').first()).toHaveCSS("color", "rgb(255, 0, 64)");
  await expect(shfe.locator('td[data-tone="down"]').first()).toHaveCSS("color", "rgb(0, 185, 143)");
  await expect(shfe.locator(".quote-activity").first()).toHaveCSS("color", "rgb(0, 194, 211)");
  await shfe.getByRole("button", { name: "rb2601", exact: true }).click();
  const detail = page.getByRole("complementary", { name: "合约详情" });
  const upper = detail.locator(".quote-pane").first();
  const lower = detail.locator(".quote-pane").last();
  await expect(upper.locator(".quote-pane-header")).toContainText("rb2601");
  await lower.getByRole("button", { name: /更多/ }).click();
  await lower.getByRole("menuitemradio", { name: "历史K线" }).click();
  const history = page.getByRole("region", { name: "合约历史 K 线", exact: true });
  await expect(history.getByRole("img")).toBeVisible();
  await expect(history.getByRole("button", { name: "5 min", exact: true })).toHaveText("5分");
  await expect(history.getByRole("button", { name: "1 min", exact: true })).toBeDisabled();
  await expect(history.getByRole("button", { name: "日K", exact: true })).toBeDisabled();
  await expect(history.locator("[data-oscillator-legend]")).toContainText("MACD:");
  await expect(history.locator("[data-mirrored-price]")).toHaveCount(5);
  await expect(history.locator("[data-price-extreme]")).toHaveCount(2);
  await expect(history.locator("[data-indicator]")).toHaveCount(5);
  await expect(history.locator('[data-oscillator="MACD(12,26,9)"]')).toHaveCount(1);
  await expect(history.locator('[data-oscillator-line="DIFF"]')).not.toHaveAttribute("d", "");
  await history.getByRole("button", { name: "显示 MACD", exact: true }).click();
  await expect(history.locator("[data-oscillator]")).toHaveCount(0);
  await history.getByRole("button", { name: "显示 MACD", exact: true }).click();
  await history.getByRole("button", { name: "显示均线", exact: true }).click();
  await expect(history.locator("[data-indicator]")).toHaveCount(0);
  await history.getByRole("button", { name: "显示均线", exact: true }).click();
  await upper.getByRole("button", { name: "Tick", exact: true }).click();
  await expect(page.getByRole("img", { name: "最近报价走势" })).toBeVisible();
  const liveChart = page.getByRole("img", { name: "最近报价走势" });
  await expect(liveChart.locator("[data-chart-volume-axis]")).toHaveCount(1);
  await expect(liveChart.locator("[data-chart-reference]")).toHaveAttribute(
    "data-chart-reference",
    "100",
  );
  await expect(liveChart.locator("[data-chart-percent]")).toHaveText([
    "+5.00%",
    "+2.50%",
    "0.00%",
    "-2.50%",
    "-5.00%",
  ]);
  const quote = subscriptions[0].quote!;
  quote.previous_settlement = "110";
  await expect(liveChart.locator("[data-chart-reference]")).toHaveAttribute(
    "data-chart-reference",
    "110",
  );
  await expect(liveChart.locator("[data-chart-percent]").nth(2)).toHaveText("0.00%");
  for (const invalid of ["0", "-1", "invalid", null]) {
    quote.previous_settlement = invalid;
    await expect(liveChart.locator("[data-chart-percent]")).toHaveCount(0);
    await expect(liveChart.locator("[data-chart-reference]")).toHaveCount(0);
    await expect(liveChart.locator(".price-chart-axis-tick")).toHaveCount(5);
  }
  quote.previous_settlement = "100";
  await expect(liveChart.locator("[data-chart-percent]")).toHaveCount(5);

  expect(await liveChart.locator("[data-chart-volume]").count()).toBeGreaterThan(0);
  const cursor = upper.locator(".contract-live-chart .price-chart-cursor");
  await expect(cursor).toContainText("区间成交量");
  const firstQuote = await liveChart.locator("polyline").evaluate(line => {
    const point = (line as SVGPolylineElement).points.getItem(0);
    const screen = new DOMPoint(point.x, point.y).matrixTransform(
      (line as SVGPolylineElement).getScreenCTM()!,
    );
    return { x: screen.x, y: screen.y };
  });
  await page.mouse.move(firstQuote.x, firstQuote.y);
  await expect(cursor).toContainText("100.00");
  await expect(cursor).not.toContainText("区间成交量");
  await liveChart.focus();
  await page.keyboard.press("End");
  await expect(cursor).toContainText("区间成交量");
  const assertColumnAlignment = async () => {
    const offsets = await page.locator(".market-exchange-groups table").evaluateAll(tables =>
      tables.flatMap(table => {
        const headings = [...table.querySelectorAll("thead th")];
        const cells = [...table.querySelectorAll("tbody tr:first-child td")];
        return headings.map((heading, index) => {
          const label = heading.querySelector(".quote-heading-label")!;
          const cell = cells[index];
          const range = document.createRange();
          range.selectNodeContents(
            index === 0
              ? cell.querySelector(".quote-select")!
              : (cell.querySelector(".quote-status-label") ?? cell),
          );
          const edge = cell.classList.contains("numeric") ? "right" : "left";
          const headingRect = label.getBoundingClientRect();
          const valueRect = range.getBoundingClientRect();
          return {
            column: label.textContent,
            offset: Math.abs(
              cell.classList.contains("quote-status")
                ? (headingRect.left + headingRect.right - valueRect.left - valueRect.right) / 2
                : headingRect[edge] - valueRect[edge],
            ),
          };
        });
      }),
    );
    for (const column of offsets)
      expect(
        column.offset,
        `Column ${column.column} must share its value's alignment edge`,
      ).toBeLessThan(1);
  };
  await assertColumnAlignment();
  await expect(shfe.getByRole("cell", { name: "-5.00%", exact: true }).first()).toBeVisible();
  await shfe.getByRole("button", { name: "最新价", exact: true }).click();
  await expect(shfe.locator("tbody tr").first()).toContainText("rb2608");
  await assertColumnAlignment();
  await expect(upper.locator(".quote-pane-header")).toContainText("rb2601");
  await page.locator(".market-exchange-groups").evaluate(element => element.scrollTo(0, 0));
  const footer = await page.locator(".status-bar").boundingBox();
  const candle = await history.getByRole("img").boundingBox();
  expect(candle!.y + candle!.height).toBeLessThanOrEqual(footer!.y);
  const rowHeight = await shfe
    .locator("tbody tr")
    .first()
    .evaluate(element => element.getBoundingClientRect().height);
  expect(rowHeight).toBeLessThanOrEqual(26);
  await page.screenshot({ path: "build/market-reference-linked-charts.png" });
  await page.setViewportSize({ width: 2048, height: 1073 });
  await assertColumnAlignment();
  await expect(history.locator(".price-chart-axis-tick")).toHaveCount(5);
  const shanghaiGroup = await shfe.boundingBox();
  const zhengzhouGroup = await page.getByRole("region", { name: "郑州商品期货" }).boundingBox();
  expect(shanghaiGroup!.y).toBeLessThan(zhengzhouGroup!.y);
  await expect(shfe.getByRole("columnheader", { name: "状态", exact: true })).toHaveCount(0);
  const boardTop = await page.locator(".futures-market-board-list").boundingBox();
  const chartTop = await page.locator(".market-contract").boundingBox();
  expect(Math.abs(boardTop!.y - chartTop!.y)).toBeLessThan(2);
  await page.getByRole("button", { name: "行情设置" }).click();
  await menu.getByText("行情连接", { exact: true }).click();
  expect((await page.locator(".market-contract").boundingBox())!.y).toBe(chartTop!.y);
  await menu.getByText("行情连接", { exact: true }).click();
  await page.keyboard.press("Escape");
  const largePlot = await history.getByRole("img").boundingBox();
  expect(largePlot!.height).toBeGreaterThan(candle!.height);
  const largeFooter = await page.locator(".status-bar").boundingBox();
  expect(largePlot!.y + largePlot!.height).toBeLessThan(largeFooter!.y);
  await page.screenshot({ path: "build/market-reference-2048.png" });
  await history.getByRole("button", { name: "15 min", exact: true }).click();
  await expect(history.getByRole("button", { name: "15 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect.poll(() => historyPage?.id).toBe("history-15min");
  await expect(history.getByRole("img")).toBeVisible();
  await history.getByRole("button", { name: "5 min", exact: true }).click();
  await expect.poll(() => historyPage?.id).toBe("linked-history-fixture");
  await expect(history.getByRole("img")).toBeVisible();
  fail = true;
  await history.getByRole("button", { name: "向后", exact: true }).click();
  await expect(history.getByRole("alert")).toBeVisible();
  await expect(history.getByRole("img")).toBeVisible();
  await history.getByRole("button", { name: "重试", exact: true }).click();
  await expect(history.getByText("101–120 / 120", { exact: true })).toBeVisible();
  await expect(history.locator('[data-indicator="MA60"]')).not.toHaveAttribute("d", "");
  await history.getByRole("button", { name: "最早数据", exact: true }).click();
  await expect(history.getByText("1–100 / 120", { exact: true })).toBeVisible();
  await expect(history.getByRole("button", { name: "最早数据", exact: true })).toBeDisabled();
  await expect.poll(() => historyPage?.offset).toBe(0);
  await history.getByRole("button", { name: "最新数据", exact: true }).click();
  await expect(history.getByText("101–120 / 120", { exact: true })).toBeVisible();
  await expect(history.getByRole("button", { name: "最新数据", exact: true })).toBeDisabled();
  await expect(history.locator('[data-indicator="MA60"]')).not.toHaveAttribute("d", "");
  await page.screenshot({ path: "build/history-first-last-page.png" });
  await shfe.getByRole("button", { name: "rb2602", exact: true }).click();
  await expect(history.getByRole("img")).toHaveCount(0);
  await expect(history.getByRole("button", { name: "下载历史数据", exact: true })).toBeVisible();
  await shfe.getByRole("button", { name: "rb2601", exact: true }).click();
  await expect(history.getByRole("img")).toBeVisible();
  await page.setViewportSize({ width: 800, height: 900 });
  await assertColumnAlignment();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "build/market-reference-linked-compact.png" });
  await page.setViewportSize({ width: 2048, height: 1073 });
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "自选", exact: true })
    .click();
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(44);
  await page.setViewportSize({ width: 1440, height: 700 });
  await expect(page.locator(".watchlist-charts").getByRole("img")).toHaveCount(2);
  for (const pane of await page.locator(".watchlist-chart-pane").all()) {
    const plot = pane.getByRole("img");
    const pages = pane.locator(".market-history-pages");
    await expect
      .poll(async () => {
        const graph = (await plot.boundingBox())!;
        const controls = (await pages.boundingBox())!;
        return controls.y - (graph.y + graph.height);
      })
      .toBeGreaterThanOrEqual(0);
    await pages.scrollIntoViewIfNeeded();
    await expect(pages.getByRole("button", { name: "最新数据", exact: true })).toBeInViewport();
  }
  await page.screenshot({ path: "build/watchlist-short-window-charts.png" });
  const tableScroll = page.locator(".watchlist-table-scroll");
  const headingTop = (await page.locator(".watchlist-table th").first().boundingBox())!.y;
  await tableScroll.evaluate(element => {
    element.scrollTop = 400;
  });
  await expect.poll(() => tableScroll.evaluate(element => element.scrollTop)).toBeGreaterThan(0);
  await expect
    .poll(async () => (await page.locator(".watchlist-table th").first().boundingBox())!.y)
    .toBeCloseTo(headingTop, 0);
  await page.screenshot({ path: "build/watchlist-sticky-headings.png" });
  await tableScroll.evaluate(element => {
    element.scrollTop = 0;
  });
  const pinnedCode = page.locator(".watchlist-table tbody tr").first().locator("td").nth(1);
  const pinnedX = (await pinnedCode.boundingBox())!.x;
  await tableScroll.evaluate(element => {
    element.scrollLeft = element.scrollWidth;
  });
  await expect.poll(() => tableScroll.evaluate(element => element.scrollLeft)).toBeGreaterThan(0);
  expect((await pinnedCode.boundingBox())!.x).toBeCloseTo(pinnedX, 0);
  expect((await page.locator(".watchlist-table th").nth(1).boundingBox())!.x).toBeCloseTo(
    pinnedX,
    0,
  );
  await expect(
    page.locator(".watchlist-table").getByRole("button", { name: "rb2601", exact: true }),
  ).toBeInViewport();
  await page.screenshot({ path: "build/watchlist-pinned-contract.png" });
  await tableScroll.evaluate(element => {
    element.scrollLeft = 0;
  });
  await page.setViewportSize({ width: 2048, height: 1073 });
  for (const plot of await page.locator(".watchlist-charts").getByRole("img").all()) {
    await expect.poll(async () => (await plot.boundingBox())!.height).toBeGreaterThan(250);
  }
  // Measure rendered text, not just cells: global button flex centering can
  // misalign a heading even when its cell has the correct text-align value.
  const textOffsets = await page.locator(".watchlist-table").evaluate(table => {
    const edge = (element: Element, left: boolean) => {
      const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
      const rects: { left: number; right: number }[] = [];
      for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        if (!node.textContent?.trim()) continue;
        const range = document.createRange();
        range.selectNodeContents(node);
        const bounds = range.getBoundingClientRect();
        const parent = node.parentElement!;
        if (getComputedStyle(parent).overflow === "hidden") {
          // Range includes invisible text beyond CSS ellipsis; measure its visible edge.
          const clip = parent.getBoundingClientRect();
          rects.push({
            left: Math.max(bounds.left, clip.left),
            right: Math.min(bounds.right, clip.right),
          });
        } else rects.push(bounds);
      }
      return left ? Math.min(...rects.map(r => r.left)) : Math.max(...rects.map(r => r.right));
    };
    const headings = [...table.querySelectorAll("th")];
    const cells = [...table.querySelectorAll("tbody tr:first-child td")];
    return headings.map((heading, index) => {
      const left = index === 1 || index === 2;
      return Math.abs(edge(heading, left) - edge(cells[index], left));
    });
  });
  for (const [index, offset] of textOffsets.entries())
    expect(offset, `Column ${index}`).toBeLessThan(1);
  const priceHeading = page.locator(".watchlist-table th").filter({ hasText: /^现价$/ });
  const unsupportedHeading = page.locator(".watchlist-table th").filter({ hasText: /^主力净额$/ });
  await expect(unsupportedHeading.getByRole("button")).toBeDisabled();
  await expect(unsupportedHeading).not.toHaveAttribute("aria-sort");
  await expect(page.locator(".watchlist-table th button:not(:disabled)")).toHaveCount(10);
  await expect(page.locator(".watchlist-table tbody tr").first().locator("td").nth(15)).toHaveText(
    "98.12500001",
  );
  const previousCloseCell = page
    .locator(".watchlist-table tbody tr")
    .first()
    .locator("td")
    .nth(15)
    .locator("span");
  await expect(previousCloseCell).toHaveAttribute("title", "98.12500001");
  // A value too long for its column is cut with an ellipsis and stays
  // readable through its title. Whether this one is cut depends on the
  // window width, so only the rule is checked.
  expect(
    await previousCloseCell.evaluate(element => ({
      overflow: getComputedStyle(element).overflow,
      ellipsis: getComputedStyle(element).textOverflow,
    })),
  ).toEqual({ overflow: "hidden", ellipsis: "ellipsis" });
  const openHeading = page.locator(".watchlist-table th").filter({ hasText: /^开盘$/ });
  await openHeading.getByRole("button").click();
  await expect(openHeading).toHaveAttribute("aria-sort", "descending");
  const opens = await page
    .locator(".watchlist-table tbody tr")
    .evaluateAll(rows => rows.map(row => row.children[14].textContent));
  const availableOpens = opens.filter(value => value !== "—").map(Number);
  expect(availableOpens).toEqual([...availableOpens].sort((a, b) => b - a));
  expect(opens.slice(availableOpens.length).every(value => value === "—")).toBe(true);
  await priceHeading.getByRole("button").click();
  await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
  expect(
    await priceHeading
      .locator("button > span")
      .evaluate(element => getComputedStyle(element, "::before").content),
  ).toContain("↓");
  const prices = () =>
    page
      .locator(".watchlist-table tbody tr")
      .evaluateAll(rows => rows.map(row => Number(row.children[4].textContent)));
  expect(await prices()).toEqual([...(await prices())].sort((a, b) => b - a));
  await priceHeading.getByRole("button").click();
  await expect(priceHeading).toHaveAttribute("aria-sort", "ascending");
  expect(await prices()).toEqual([...(await prices())].sort((a, b) => a - b));
  const sortedCodes = await page
    .locator(".watchlist-table tbody tr td:nth-child(2)")
    .allTextContents();
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2602", exact: true })
    .dblclick();
  await expect(page.locator(".contract-summary")).toContainText("rb2602");
  await page.getByRole("button", { name: "全部自选", exact: true }).click();
  await expect(priceHeading).toHaveAttribute("aria-sort", "ascending");
  await expect(page.locator(".watchlist-table tbody tr td:nth-child(2)")).toHaveText(sortedCodes);
  await expect(page.locator(".watchlist-table tbody tr[data-selected=true]")).toContainText(
    "rb2602",
  );
  await page.screenshot({ path: "build/watchlist-retained-sort.png" });
  await page.screenshot({ path: "build/watchlist-sort-direction.png" });
  const secondContract = page.locator(".watchlist-table tbody tr").filter({ hasText: "rb2602" });
  await secondContract.locator("td").first().click();
  await expect(secondContract).toHaveAttribute("data-selected", "true");
  await expect(secondContract.getByRole("button")).toBeFocused();
  const watchlistRows = page.locator(".watchlist-table tbody tr");
  await page.keyboard.press("End");
  await expect(watchlistRows.last()).toHaveAttribute("data-selected", "true");
  await expect(watchlistRows.last().getByRole("button")).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(watchlistRows.last().getByRole("button")).toBeFocused();
  await page.keyboard.press("Home");
  await expect(watchlistRows.first()).toHaveAttribute("data-selected", "true");
  await expect(watchlistRows.first().getByRole("button")).toBeFocused();
  await page.keyboard.press("ArrowUp");
  await expect(watchlistRows.first().getByRole("button")).toBeFocused();
  await secondContract.locator("td").nth(4).click();
  await expect(secondContract.getByRole("button")).toBeFocused();
  await page.screenshot({ path: "build/watchlist-keyboard-selection.png" });
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2601", exact: true })
    .click();
  await expect(page.locator(".watchlist-charts").getByRole("img")).toHaveCount(2);
  const sheet = await page.locator(".watchlist-sheet").boundingBox();
  const plots = await page.locator(".watchlist-charts").boundingBox();
  expect(sheet!.width / plots!.width).toBeCloseTo(3, 0);
  const upperChart = page.locator(".watchlist-chart-pane").first();
  const lowerChart = page.locator(".watchlist-chart-pane").last();
  await expect(upperChart.getByRole("button", { name: "5 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(lowerChart.getByRole("button", { name: "15 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await upperChart.getByRole("button", { name: "15 min", exact: true }).click();
  await expect(lowerChart.getByRole("img")).toBeVisible();
  await upperChart.getByRole("button", { name: "5 min", exact: true }).click();
  await expect(upperChart.getByRole("img")).toBeVisible();
  await page.screenshot({ path: "build/watchlist-reference-2048.png" });
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2601", exact: true })
    .dblclick();
  await expect(page.locator(".contract-workspace")).toBeVisible();
  await expect(page.locator(".contract-summary")).toContainText("rb2601");
  const depthRatio = page.getByRole("img", { name: "买一卖一挂单量占比", exact: true });
  await expect(depthRatio).toHaveAttribute("data-empty", "false");
  expect(await depthRatio.locator("span").evaluate(element => element.style.width)).toBe("60%");
  await expect(page.locator(".contract-center").getByRole("img")).toBeVisible();
  const livePlot = page.locator(".contract-live-chart").getByRole("img");
  const liveReadout = page.locator(".contract-live-chart .price-chart-cursor");
  await expect(liveReadout).toContainText("区间成交量 52");
  const liveBounds = (await livePlot.boundingBox())!;
  await page.mouse.move(liveBounds.x + 1, liveBounds.y + liveBounds.height / 2);
  await expect(liveReadout).not.toContainText("区间成交量");
  await livePlot.focus();
  await page.keyboard.press("ArrowRight");
  await expect(liveReadout).toContainText("区间成交量 13");
  await page.screenshot({ path: "build/contract-quote-volume-readout.png" });
  const beforeRollover = await livePlot.locator("polyline").getAttribute("points");
  quoteHistoryOffset = 1;
  await expect(livePlot.locator("polyline")).not.toHaveAttribute("points", beforeRollover!);
  await expect(liveReadout).toContainText("区间成交量 13");
  // When the selected event is evicted, return to the latest visible event.
  quoteHistoryOffset = 2;
  await expect(liveReadout).toContainText("区间成交量 58");
  await livePlot.press("End");
  quoteHistoryOffset = 3;
  await expect(liveReadout).toContainText("区间成交量 10");
  quoteHistoryOffset = 0;
  await expect(liveReadout).toContainText("区间成交量 52");

  const sparklines = page.locator(".contract-list-row svg polyline");
  await expect(sparklines).toHaveCount(1);
  marketOnline = false;
  await expect(sparklines).toHaveCount(0);
  await expect(page.locator(".contract-live-chart")).toContainText("报价序列暂不可用");
  await page.screenshot({ path: "build/contract-offline-sparklines.png" });
  marketOnline = true;
  await expect(sparklines).toHaveCount(1);
  quoteHistoryAvailable = false;
  await expect(sparklines).toHaveCount(0);
  await expect(page.locator(".contract-live-chart")).toContainText("报价序列暂不可用");
  quoteHistoryAvailable = true;
  await expect(sparklines).toHaveCount(1);
  await expect(livePlot).toBeVisible();
  await expect(page.locator(".contract-depth")).toContainText("当前报价未提供更多档位");
  for (const row of subscriptions) {
    row.quote!.open_interest_change = "-25.25000001";
    row.quote!.open = "3490.25000001";
    row.quote!.upper_limit = "3800.5";
    row.quote!.lower_limit = "0";
    row.quote!.bid_levels = [
      { price: "3508.25", quantity: 4 },
      { price: null, quantity: null },
      { price: "3506", quantity: null },
      { price: "3505", quantity: 10 },
    ];
    row.quote!.ask_levels = [
      { price: "3512.5", quantity: 5 },
      { price: "3513", quantity: 0 },
      { price: null, quantity: null },
      { price: "3515", quantity: 11 },
    ];
  }
  const sessionPrice = (label: string) =>
    page
      .locator(".contract-summary dl div")
      .filter({ has: page.locator("dt", { hasText: new RegExp(`^${label}$`) }) })
      .locator("dd");
  await expect(sessionPrice("日增仓")).toHaveText("-25.25000001");
  await expect(sessionPrice("开盘")).toHaveText("3490.25000001");
  await expect(sessionPrice("涨停")).toHaveText("3800.5");
  await expect(sessionPrice("跌停")).toHaveText("0");
  const depth = page.locator(".contract-depth");
  await expect(depth.locator(".depth-level").nth(1)).toContainText("3508.25");
  await expect(depth.locator(".depth-level").nth(1)).toContainText("3512.5");
  await expect(depth.locator(".depth-level").nth(2).locator("span").last()).toHaveText("0");
  await expect(depth.locator(".depth-level").nth(3).locator("span").first()).toHaveText("—");
  await expect(depth.locator(".depth-level").nth(4)).toContainText("3515");
  await expect(depth).not.toContainText("当前报价未提供更多档位");
  await page.screenshot({ path: "build/contract-depth-five-level.png" });
  for (const row of subscriptions) {
    row.quote!.open_interest_change = null;
    row.quote!.open = null;
    row.quote!.upper_limit = null;
    row.quote!.lower_limit = null;
    row.quote!.bid_levels = Array.from({ length: 4 }, () => ({ price: null, quantity: null }));
    row.quote!.ask_levels = Array.from({ length: 4 }, () => ({ price: null, quantity: null }));
  }
  for (const label of ["开盘", "涨停", "跌停", "日增仓"])
    await expect(sessionPrice(label)).toHaveText("—");
  await expect(depth).toContainText("当前报价未提供更多档位");
  await expect(depth.locator(".depth-level").nth(1).locator("b").first()).toHaveText("—");

  await expect(page.locator(".contract-ticks")).toContainText("逐笔成交数据尚未接入");
  const contractRows = page.locator(".contract-list-row");
  const contractHeading = page.locator(".contract-list-heading");
  const contractHeadingTop = (await contractHeading.boundingBox())!.y;
  await contractRows.first().focus();
  await page.keyboard.press("ArrowDown");
  await expect(contractRows.nth(1)).toBeFocused();
  await expect(contractRows.nth(1)).toHaveAttribute("aria-pressed", "true");
  await expect(page.locator(".contract-summary")).toContainText("rb2602");
  await page.keyboard.press("End");
  await expect(contractRows.last()).toBeFocused();
  await expect(contractRows.last()).toHaveAttribute("aria-pressed", "true");
  expect((await contractHeading.boundingBox())!.y).toBeCloseTo(contractHeadingTop, 0);
  await page.screenshot({ path: "build/contract-sticky-headings.png" });
  await page.keyboard.press("ArrowDown");
  await expect(contractRows.last()).toBeFocused();
  await page.keyboard.press("Home");
  await expect(contractRows.first()).toBeFocused();
  await expect(page.locator(".contract-summary")).toContainText("rb2601");
  const fixedHeadingBounds = (await contractHeading.boundingBox())!;
  expect((await contractRows.first().boundingBox())!.y).toBeGreaterThanOrEqual(
    fixedHeadingBounds.y + fixedHeadingBounds.height,
  );
  await page.keyboard.press("ArrowUp");
  await expect(contractRows.first()).toBeFocused();
  const leftPane = await page.locator(".contract-watchlist").boundingBox();
  const centerPane = await page.locator(".contract-center").boundingBox();
  const rightPane = await page.locator(".contract-right").boundingBox();
  expect(centerPane!.x).toBeGreaterThan(leftPane!.x + leftPane!.width);
  expect(rightPane!.x).toBeGreaterThan(centerPane!.x + centerPane!.width);
  await page.screenshot({ path: "build/contract-reference-2048.png" });
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "市场", exact: true })
    .click();
  historicalOnly = true;
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await expect(page.getByRole("button", { name: "SHFE · rb/2026-01", exact: false })).toBeVisible();
  await expect(history.getByRole("img")).toBeVisible();
  await history.getByRole("button", { name: "15 min", exact: true }).click();
  await expect.poll(() => historyPage?.id).toBe("history-15min");
  await expect(history.getByRole("img")).toBeVisible();
  await page.getByRole("searchbox", { name: "搜索历史合约" }).fill("unknown");
  await expect(history).toHaveCount(0);
  await expect(
    page.locator(".history-market-contracts").getByText("没有匹配的历史合约", { exact: true }),
  ).toBeVisible();
  await page.getByRole("searchbox", { name: "搜索历史合约" }).fill("RB");
  await expect(history.getByRole("img")).toBeVisible();
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.screenshot({ path: "build/market-history-macd.png" });
  // Six-row visual fixture matches the reference density without shipping sample data.
  historicalOnly = false;
  subscriptions.splice(6);
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "自选", exact: true })
    .click();
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(6);
  await page.setViewportSize({ width: 2048, height: 1073 });
  await expect(page.locator(".watchlist-charts").getByRole("img")).toHaveCount(2);
  await page.screenshot({ path: "build/watchlist-reference-2048.png" });
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2601", exact: true })
    .dblclick();
  await page.locator(".contract-list-row").filter({ hasText: "rb2602" }).click();
  await expect(page.locator(".contract-summary")).toContainText("rb2602");
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "自选", exact: true })
    .click();
  await page.getByRole("button", { name: "最近浏览", exact: true }).click();
  await expect(priceHeading).not.toHaveAttribute("aria-sort");
  await expect(page.locator(".watchlist-table tbody tr").first()).toContainText("rb2602");
  await expect(page.locator('.watchlist-table tr[data-selected="true"]')).toContainText("rb2602");
  await page
    .getByRole("navigation", { name: "自选视图" })
    .getByRole("button", { name: "自选", exact: true })
    .click();
  await expect(priceHeading).toHaveAttribute("aria-sort", "ascending");
  expect(await prices()).toEqual([...(await prices())].sort((a, b) => a - b));
  await page.getByRole("button", { name: "最近浏览", exact: true }).click();
  await expect(page.locator(".watchlist-table tbody tr").first()).toContainText("rb2602");
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2601", exact: true })
    .dblclick();
  await expect(page.locator(".contract-summary")).toContainText("rb2601");
  await expect(page.locator(".contract-center").getByRole("img")).toBeVisible();
  await page.screenshot({ path: "build/contract-reference-2048.png" });
  expect(pageErrors).toEqual([]);
  catalogNames = true;
  await expect(page.locator(".contract-list-row").first()).toContainText("螺纹钢2601");
  const firstContract = page.locator(".contract-list-row").first();
  const priceBeforeNameChange = (await firstContract
    .locator(":scope > span")
    .last()
    .boundingBox())!;
  catalogLabel = "螺纹钢2601 · 用于验证长名称显示边界的测试目录名称";
  await expect(firstContract.locator("b")).toHaveText(catalogLabel);
  const priceAfterNameChange = (await firstContract.locator(":scope > span").last().boundingBox())!;
  expect(priceAfterNameChange.x).toBeCloseTo(priceBeforeNameChange.x, 0);
  expect(priceAfterNameChange.width).toBeCloseTo(priceBeforeNameChange.width, 0);
  await expect(firstContract.locator("b")).toHaveAttribute("title", catalogLabel);
  const nameOverflow = await firstContract.locator("b").evaluate(element => ({
    clipped: element.scrollWidth > element.clientWidth,
    overflow: getComputedStyle(element).textOverflow,
  }));
  expect(nameOverflow).toEqual({ clipped: true, overflow: "ellipsis" });
  await page.screenshot({ path: "build/contract-long-name.png" });
  catalogLabel = "螺纹钢2601";
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "自选", exact: true })
    .click();
  await expect(
    page.locator(".watchlist-table tbody tr").filter({ hasText: "rb2601" }),
  ).toContainText("螺纹钢2601");
  await page.getByRole("searchbox", { name: "搜索自选合约" }).fill("螺纹钢");
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(1);
  await page.screenshot({ path: "build/watchlist-catalog-name.png" });
  marketOnline = false;
  await expect(page.locator(".watchlist-chart-pane .quote-freshness")).toHaveCount(2);
  await expect(page.locator(".watchlist-chart-pane .quote-freshness").first()).toHaveText(
    "断线 · 旧报价",
  );
  await expect(page.locator(".watchlist-charts").getByRole("img")).toHaveCount(2);
  await page.screenshot({ path: "build/watchlist-stale-quote.png" });
  marketOnline = true;
  // Cleared by the next status poll, which a loaded machine delays.
  await expect(page.locator(".watchlist-chart-pane .quote-freshness")).toHaveCount(0, {
    timeout: 15000,
  });
  await page.getByRole("searchbox", { name: "搜索自选合约" }).fill("no-such-contract");
  await expect(page.getByText("没有匹配的自选合约", { exact: true })).toBeVisible();
  await expect(page.getByText("尚无订阅行情", { exact: true })).toHaveCount(0);
  await page.getByRole("button", { name: "清除搜索", exact: true }).click();
  await expect(page.getByRole("searchbox", { name: "搜索自选合约" })).toHaveValue("");
  await expect(page.locator('.watchlist-table tr[data-selected="true"]')).toContainText("rb2601");
  await page.getByRole("button", { name: "最近浏览", exact: true }).click();
  await page.getByRole("searchbox", { name: "搜索自选合约" }).fill("rb2605");
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(0);
  await expect(page.getByText("没有匹配的自选合约", { exact: true })).toBeVisible();
  await expect(page.locator(".watchlist-chart-pane")).toHaveCount(0);
  await expect(page.locator(".watchlist-charts")).toContainText("选择合约查看图表");
  await page.screenshot({ path: "build/recent-search-empty-charts.png" });
  await page
    .getByRole("navigation", { name: "自选视图" })
    .getByRole("button", { name: "自选", exact: true })
    .click();
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(1);
  await expect(page.locator(".watchlist-chart-pane").first()).toContainText("rb2605");
  await page.getByRole("button", { name: "最近浏览", exact: true }).click();
  await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(0);
  await expect(page.locator(".watchlist-chart-pane")).toHaveCount(0);
  await page.getByRole("button", { name: "清除搜索", exact: true }).click();
  await expect(page.locator('.watchlist-table tr[data-selected="true"]')).toContainText("rb2601");
  await expect(page.locator(".watchlist-charts").getByRole("img")).toHaveCount(2);
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "合约", exact: true })
    .click();
  await page.getByRole("searchbox", { name: "搜索自选合约" }).fill("no-such-contract");
  await expect(page.getByText("没有匹配的自选合约", { exact: true })).toBeVisible();
  await expect(
    page.locator(".contract-watchlist").getByRole("button", { name: "连接行情" }),
  ).toHaveCount(0);
  await page.screenshot({ path: "build/contract-search-empty.png" });
  await page.getByRole("button", { name: "清除搜索", exact: true }).click();
  await expect(page.locator(".contract-summary")).toContainText("螺纹钢2601");
  await page.getByRole("searchbox", { name: "搜索自选合约" }).fill("");
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "市场", exact: true })
    .click();
  await expect(page.locator(".quote-select").filter({ hasText: "螺纹钢2601" })).toHaveCount(1);
});
