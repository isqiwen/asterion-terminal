import { test, expect } from "./test";
import type { HistoryPage, Snapshot } from "../src/bridge/client";

test("chart panes retain intervals and indicators across contracts without substituting missing data", async ({
  page,
}) => {
  const identity = (symbol: string) => `SHFE/rb/20${symbol.slice(-4, -2)}-${symbol.slice(-2)}`;
  const requests: string[] = [];
  const pageRequests: { id: string; offset: number }[] = [];
  let online = true;
  let service = "fixture-a";
  let catalogName = "";
  const start = 1790582400000000000n;
  const datasets: { symbol: string; period: number; rows?: number; version?: number }[] = [
    { symbol: "rb2610", period: 1, rows: 250 },
    { symbol: "rb2610", period: 5 },
    { symbol: "rb2701", period: 1 },
    { symbol: "rb2701", period: 5 },
    { symbol: "rb2705", period: 1 },
  ];
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!["runtime.snapshot", "research.minutes.page"].includes(request.method))
      return route.continue();
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (catalogName)
      data.result.history_contracts = {
        source: "tushare.ft_mins",
        exchange: "SHFE",
        product: "RB",
        connection: "",
        connection_revision: "",
        cutoff_ns: "0",
        items: [
          {
            code: "SHFE/rb/2027-05",
            name: catalogName,
            list_date: "20260101",
            delist_date: "20270515",
            multiplier: null,
            per_unit: "5.00000001",
            trade_unit: "吨",
            quote_unit: "元/吨",
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
        contracts: ["rb2610", "rb2701", "rb2705"].map(symbol => ({
          venue: "SHFE",
          symbol,
          product: "rb",
          expiry: "20270515",
          contract_id: identity(symbol),
          multiplier: 10,
          price_tick: "1",
          name: "",
        })),
      },
      transport_online: true,
      phase: "connected",
      watchlist: ["rb2610", "rb2701", "rb2705"].map(symbol => ({ venue: "SHFE", symbol })),
      subscriptions: ["rb2610", "rb2701", "rb2705"].map(symbol => ({
        venue: "SHFE",
        symbol,
        state: "subscribed",
        error_code: 0,
        quote: null,
      })),
    };
    data.result.research = {
      ...data.result.research!,
      service,
      connection_id: service,
      online,
      tasks: datasets.map(({ symbol, period, version }, index) => ({
        id: `${symbol}-${period}${version ? `-v${version}` : ""}`,
        kind: "minute_download",
        data_source: "tushare.ft_mins",
        state: "succeeded",
        attempt: 1,
        instrument: identity(symbol),
        minute_interval_minutes: period,
        submission_sequence: 10 - index,
        source_name: "Explicit UI fixture",
        result_digest: "b".repeat(64),
        completed: 1,
        total: 1,
        error: "",
        trading_day: "",
        submitted_at_ms: 1,
        updated_at_ms: 1,
      })),
    };
    data.result.history_page = null;
    if (request.method === "research.minutes.page") {
      requests.push(request.params.id);
      pageRequests.push({ id: request.params.id, offset: request.params.offset });
      const dataset = datasets.find(
        d => `${d.symbol}-${d.period}${d.version ? `-v${d.version}` : ""}` === request.params.id,
      )!;
      expect(dataset).toBeDefined();
      const count = dataset.rows ?? 80;
      const bars = Array.from(
        { length: Math.max(0, Math.min(request.params.limit, count - request.params.offset)) },
        (_, localIndex) => {
          const index = request.params.offset + localIndex;
          return {
            timestamp_ns: String(start + BigInt(index * dataset.period) * 60000000000n),
            open: String(100 + index),
            high: String(102 + index),
            low: String(99 + index),
            close: String(101 + index),
            volume: "10",
            amount: "1000",
            open_interest: "100",
            ...(index >= 33 ? { macd: { diff: 1, signal: 0.5, histogram: 1 } } : {}),
          };
        },
      );
      data.result.history_page = {
        id: request.params.id,
        source: "tushare.ft_mins",
        contract_id: identity(dataset.symbol),
        interval_minutes: dataset.period,
        manifest_sha256: "b".repeat(64),
        total_rows: count,
        matched_rows: count,
        offset: request.params.offset,
        limit: request.params.limit,
        begin_ns: String(start),
        first_ns: String(start),
        end_ns: String(start + BigInt((count - 1) * dataset.period) * 60000000000n),
        last_ns: String(start + BigInt((count - 1) * dataset.period) * 60000000000n),
        bars,
      } satisfies HistoryPage;
    }
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  const nav = page.getByRole("navigation", { name: "业务工作区" });
  await nav.getByRole("button", { name: "自选", exact: true }).click();
  const upper = page.locator(".watchlist-chart-pane").nth(0);
  const lower = page.locator(".watchlist-chart-pane").nth(1);
  await expect(upper.getByRole("img")).toBeVisible();
  await upper.getByRole("button", { name: "向后", exact: true }).click();
  await expect(upper.locator(".market-history-pages")).toContainText("101–200 / 250");
  datasets.unshift({ symbol: "rb2610", period: 1, rows: 20, version: 2 });
  await expect(upper.locator(".market-history-pages")).toContainText("1–20 / 20");
  expect(
    pageRequests.filter(request => request.id === "rb2610-1-v2").map(request => request.offset),
  ).toEqual([0]);

  await upper.getByText("数据版本", { exact: true }).click();
  await upper.getByRole("combobox", { name: "历史数据版本" }).selectOption("rb2610-1");
  await expect(upper.locator(".market-history-pages")).toContainText("1–100 / 250");
  await upper.getByRole("button", { name: "向后", exact: true }).click();
  await expect(upper.locator(".market-history-pages")).toContainText("101–200 / 250");
  datasets.unshift({ symbol: "rb2610", period: 1, rows: 5, version: 3 });
  await expect(
    upper.getByRole("combobox", { name: "历史数据版本" }).locator("option[value='rb2610-1-v3']"),
  ).toHaveCount(1);
  await expect(upper.getByRole("combobox", { name: "历史数据版本" })).toHaveValue("rb2610-1");
  await expect(upper.locator(".market-history-pages")).toContainText("101–200 / 250");
  expect(requests).not.toContain("rb2610-1-v3");
  datasets.find(
    dataset => dataset.symbol === "rb2610" && dataset.period === 1 && !dataset.version,
  )!.rows = 10;
  const beforeSwitch = pageRequests.length;
  service = "fixture-b";
  await expect(upper.locator(".market-history-pages")).toContainText("1–5 / 5");
  await expect(upper.getByRole("combobox", { name: "历史数据版本" })).toHaveValue("rb2610-1-v3");
  expect(
    pageRequests
      .slice(beforeSwitch)
      .filter(request => request.id === "rb2610-1-v3")
      .map(request => request.offset),
  ).toEqual([0]);
  // A same-named dataset is a new selection on the new connection.
  await upper.getByRole("combobox", { name: "历史数据版本" }).selectOption("rb2610-1");
  await expect(upper.locator(".market-history-pages")).toContainText("1–10 / 10");
  await upper.getByText("数据版本", { exact: true }).click();
  await expect(lower.getByRole("button", { name: "5 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await upper.getByRole("button", { name: "5 min", exact: true }).click();
  await upper.getByRole("button", { name: "显示均线", exact: true }).click();
  await upper.getByRole("button", { name: "显示成交量", exact: true }).click();
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2701", exact: true })
    .click();
  await expect(upper.getByRole("button", { name: "5 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(upper.getByRole("img")).toBeVisible();
  await expect(upper.locator("[data-indicator]")).toHaveCount(0);
  await expect(upper.locator("[data-oscillator]")).toHaveCount(0);
  await expect(lower.locator("[data-indicator]")).toHaveCount(5);
  await expect(lower.locator("[data-oscillator]")).toHaveCount(1);
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2705", exact: true })
    .click();
  for (const pane of [upper, lower]) {
    await expect(pane.locator(".market-chart-empty[role=status]")).toContainText(
      "该合约尚无所选周期的数据",
    );
    await expect(pane.getByRole("img")).toHaveCount(0);
    await expect(pane.getByRole("button", { name: "5 min", exact: true })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
  }
  expect(requests).not.toContain("rb2705-1");
  await upper.getByRole("button", { name: "1 min", exact: true }).click();
  await expect(upper.getByRole("img")).toBeVisible();
  await expect(lower.getByRole("img")).toHaveCount(0);
  await page
    .locator(".watchlist-table")
    .getByRole("button", { name: "rb2610", exact: true })
    .click();
  await expect(upper.getByRole("button", { name: "1 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(lower.getByRole("button", { name: "5 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await nav.getByRole("button", { name: "合约", exact: true }).click();
  await page.getByRole("button", { name: "历史 K 线", exact: true }).click();
  const contract = page.locator(".contract-chart-panel");
  const contractPlot = contract.getByRole("img", { name: "合约历史 K 线", exact: true });
  await expect(contractPlot).toBeVisible();
  await expect
    .poll(() =>
      contractPlot.evaluate(element => {
        const svg = element as SVGSVGElement;
        return Math.abs(svg.viewBox.baseVal.height - svg.getBoundingClientRect().height);
      }),
    )
    .toBeLessThan(1);
  const emptyDepth = page.getByRole("img", { name: "暂无买卖盘数量", exact: true });
  await expect(emptyDepth).toHaveAttribute("data-empty", "true");
  await expect(emptyDepth.locator("span")).toHaveCount(0);
  // Empty ratio uses the raised surface token (#11161d).
  await expect(emptyDepth).toHaveCSS("background-color", "rgb(17, 22, 29)");
  await page.screenshot({ path: "build/contract-empty-depth.png" });
  await expect(contract.getByRole("button", { name: "1 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await contract.getByRole("button", { name: "5 min", exact: true }).click();
  await contract.getByRole("button", { name: "显示均线", exact: true }).click();
  await page.locator(".contract-list-row").filter({ hasText: "rb2701" }).click();
  await expect(contract.getByRole("img")).toBeVisible();
  await expect(contract.getByRole("button", { name: "5 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(contract.locator("[data-indicator]")).toHaveCount(0);
  await nav.getByRole("button", { name: "市场", exact: true }).click();
  await nav.getByRole("button", { name: "合约", exact: true }).click();
  await expect(contract.getByRole("button", { name: "5 min", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(contract.locator("[data-indicator]")).toHaveCount(0);
  const headings = page.locator(".contract-list-heading > span:visible");
  await expect(headings).toHaveText(["名称", "趋势", "现价"]);
  await page.screenshot({ path: "build/contract-pane-state.png" });
  await page.setViewportSize({ width: 2048, height: 1073 });
  await expect(headings).toHaveText(["名称", "趋势", "涨跌", "涨速", "成交量", "现价"]);
  await page.screenshot({ path: "build/contract-pane-wide.png" });
  online = false;
  await expect(contract.getByText("历史数据服务未连接", { exact: true })).toBeVisible();
  await expect(contract.getByRole("img")).toBeVisible();
  await expect(contract.getByRole("button", { name: "1 min", exact: true })).toBeDisabled();
  await page.locator(".contract-list-row").filter({ hasText: "rb2705" }).click();
  await expect(contract.locator('.market-chart-empty[role="status"]')).toHaveText(
    "历史数据服务未连接管理历史数据",
  );
  await expect(contract.getByText("该合约尚无所选周期的数据", { exact: true })).toHaveCount(0);
  await page.screenshot({ path: "build/contract-history-offline.png" });
  const count = requests.length;
  online = true;
  await expect(contract.getByText("该合约尚无所选周期的数据", { exact: true })).toBeVisible();
  expect(requests.length).toBe(count);
  await contract.getByRole("button", { name: "1 min", exact: true }).click();
  await expect(contract.getByRole("img")).toBeVisible();
  await nav.getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  const historyBoard = page.locator(".history-market-board");
  const historySearch = page.getByRole("searchbox", { name: "搜索历史合约" });
  await historySearch.fill("rb27");
  const selectedHistory = historyBoard.getByRole("button", {
    name: "SHFE · rb/2027-05",
    exact: false,
  });
  await selectedHistory.click();
  await expect(selectedHistory).toHaveAttribute("aria-pressed", "true");
  await nav.getByRole("button", { name: "自选", exact: true }).click();
  await nav.getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await expect(historySearch).toHaveValue("rb27");
  await expect(selectedHistory).toHaveAttribute("aria-pressed", "true");
  await expect(historyBoard.locator(".market-chart-title strong")).toHaveText("SHFE/rb/2027-05");
  await selectedHistory.focus();
  await page.keyboard.press("Home");
  const firstHistory = historyBoard.getByRole("button", {
    name: "SHFE · rb/2027-01",
    exact: false,
  });
  await expect(firstHistory).toBeFocused();
  await expect(firstHistory).toHaveAttribute("aria-pressed", "true");
  await expect(historyBoard.locator(".market-chart-title strong")).toHaveText("SHFE/rb/2027-01");
  await page.keyboard.press("ArrowUp");
  await expect(firstHistory).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(selectedHistory).toBeFocused();
  await expect(selectedHistory).toHaveAttribute("aria-pressed", "true");
  await page.keyboard.press("ArrowUp");
  await expect(firstHistory).toBeFocused();
  await page.keyboard.press("End");
  await expect(selectedHistory).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(selectedHistory).toBeFocused();
  await expect(historyBoard.locator(".market-chart-title strong")).toHaveText("SHFE/rb/2027-05");
  await expect(historyBoard.getByRole("img", { name: "合约历史 K 线", exact: true })).toBeVisible();
  await page.screenshot({ path: "build/history-market-retained-selection.png" });
  catalogName = "螺纹钢2705";
  await expect(selectedHistory).toContainText(catalogName);
  await historySearch.fill("螺纹钢");
  await expect(
    historyBoard.getByRole("group", { name: "历史合约" }).getByRole("button"),
  ).toHaveCount(1);
  await expect(selectedHistory).toHaveAttribute("aria-pressed", "true");
  await expect(selectedHistory).toContainText("SHFE · rb/2027-05");
  await expect(historyBoard.locator(".market-chart-title strong")).toHaveText("SHFE/rb/2027-05");
  await page.screenshot({ path: "build/history-market-contract-name.png" });
  await historySearch.fill("no-such-contract");
  await expect(historyBoard.getByRole("img")).toHaveCount(0);
  await expect(
    historyBoard.getByText("请在数据工作区通过数据源下载历史数据。", { exact: true }),
  ).toHaveCount(0);
  await expect(historyBoard.locator(".history-market-detail")).toContainText("没有匹配的历史合约");
  await page.screenshot({ path: "build/history-market-search-empty.png" });
  await historyBoard.getByRole("button", { name: "清除搜索", exact: true }).click();
  await expect(historySearch).toHaveValue("");
  await expect(selectedHistory).toHaveAttribute("aria-pressed", "true");
  await expect(historyBoard.getByRole("img", { name: "合约历史 K 线", exact: true })).toBeVisible();
  const savedDatasets = datasets.splice(0);
  online = false;
  await expect(historyBoard.locator(".history-market-detail")).toContainText("历史数据服务未连接");
  await expect(historyBoard.getByText("尚无已下载的历史合约", { exact: true })).toHaveCount(0);
  await expect(
    historyBoard.getByText("请在数据工作区通过数据源下载历史数据。", { exact: true }),
  ).toHaveCount(0);
  await expect(
    historyBoard.getByRole("button", { name: "管理历史数据", exact: true }),
  ).toBeVisible();
  await page.screenshot({ path: "build/history-market-unavailable-list.png" });
  online = true;
  await expect(historyBoard.getByText("尚无已下载的历史合约", { exact: true })).toBeVisible();
  await expect(
    historyBoard.getByText("请在数据工作区通过数据源下载历史数据。", { exact: true }),
  ).toBeVisible();
  datasets.push(...savedDatasets);
  await expect(selectedHistory).toHaveAttribute("aria-pressed", "true");
  await expect(historyBoard.getByRole("img", { name: "合约历史 K 线", exact: true })).toBeVisible();
});
