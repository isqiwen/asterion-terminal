import { test, expect } from "./test";
import type { Snapshot, LiveMarket } from "../src/bridge/client";

test("market board groups exchanges and links contract selection without inventing candles", async ({
  page,
}) => {
  let quoteCount = 0;
  let interrupted = false;
  let available = true;
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    const response = await route.fetch(
      request.method === "runtime.snapshot"
        ? {
            postData: { version: 1, method: "runtime.snapshot", params: {} },
          }
        : {},
    );
    const body = (await response.json()) as { result?: Snapshot };
    if (body.result?.market) {
      body.result.market = {
        ...body.result.market,
        // These views are specified without a contract catalog; a catalog
        // cached by an earlier session must not leak into them.
        catalog: {
          ...body.result.market.catalog,
          phase: "unconfigured",
          trading_day: "",
          contracts: [],
        },
        transport_online: true,
        phase: "connected",
        error_code: 0,
        history: {
          stream_id: "quote-fixture",
          generation: interrupted ? "2" : "1",
          available,
          interrupted,
          points: Array.from({ length: quoteCount }, (_, index) => ({
            venue: "DCE",
            symbol: "i2701",
            timestamp_ns: String(1790582400000000000n + BigInt(index) * 1000000000n),
            price: index % 2 ? "100.00000001" : "102",
          })),
        },
        watchlist: [
          { venue: "SHFE", symbol: "rb2610" },
          { venue: "DCE", symbol: "i2701" },
        ],
        subscriptions: [
          { venue: "SHFE", symbol: "rb2610", state: "subscribed", error_code: 0, quote: null },
          { venue: "DCE", symbol: "i2701", state: "subscribed", error_code: 0, quote: null },
        ],
      } as LiveMarket;
    }
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  const board = page.locator(".futures-market-board");
  const detail = page.getByRole("complementary", { name: "合约详情" });
  // Without a catalog the board lists subscribed months even in product overview.
  await expect(board.getByRole("button", { name: "rb2610", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "行情设置" }).click();
  const menu = page.getByRole("dialog", { name: "行情设置" });
  await menu.getByRole("button", { name: "全部月份", exact: true }).click();
  await page.keyboard.press("Escape");
  await expect(menu).toHaveCount(0);
  await expect(detail).toContainText("rb2610");
  await board.getByRole("button", { name: "i2701", exact: true }).click();
  await expect(detail).toContainText("i2701");
  await expect(board.getByRole("button", { name: "i2701", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  // Commodity groups follow the reference board: Shanghai left, Dalian below.
  await expect(board.getByRole("region", { name: "上海商品期货" })).toContainText("rb2610");
  await expect(board.getByRole("region", { name: "大连商品期货" })).toContainText("i2701");
  await expect(board.getByRole("region", { name: /境外商品期货|境外金融期货/ })).toHaveCount(0);
  const categories = page.getByRole("navigation", { name: "期货分类" });
  await categories.getByRole("button", { name: "上期所", exact: true }).click();
  await expect(board.getByRole("button", { name: "i2701", exact: true })).toHaveCount(0);
  await expect(detail).toContainText("rb2610");
  await expect(detail.locator(".quote-pane-status").first()).toHaveText("等待首笔");
  const navigation = page.getByRole("navigation", { name: "业务工作区" });
  await navigation.getByRole("button", { name: "自选", exact: true }).click();
  await navigation.getByRole("button", { name: "市场", exact: true }).click();
  await expect(categories.getByRole("button", { name: "上期所", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(board.getByRole("button", { name: "i2701", exact: true })).toHaveCount(0);
  await categories.getByRole("button", { name: "期货全景", exact: true }).click();
  await page.getByRole("button", { name: "行情设置" }).click();
  await menu.getByRole("button", { name: "全部月份", exact: true }).click();
  await page.keyboard.press("Escape");
  const listBox = await page.locator(".futures-market-board-list").boundingBox();
  const detailBox = await detail.boundingBox();
  expect(detailBox!.x).toBeGreaterThanOrEqual(listBox!.x + listBox!.width - 1);
  await board.getByRole("button", { name: "i2701", exact: true }).focus();
  await page.keyboard.press("ArrowUp");
  await expect(detail).toContainText("rb2610");
  await expect(board.getByRole("button", { name: "rb2610", exact: true })).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(detail).toContainText("i2701");
  const search = page.getByRole("searchbox", { name: "搜索市场合约" });
  // Moving to an already selected row must still move keyboard focus.
  await board.getByRole("button", { name: "rb2610", exact: true }).focus();
  await page.keyboard.press("ArrowDown");
  await expect(board.getByRole("button", { name: "i2701", exact: true })).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(board.getByRole("button", { name: "i2701", exact: true })).toBeFocused();
  await search.fill("RB");
  await expect(board.getByRole("button", { name: "i2701", exact: true })).toHaveCount(0);
  await expect(detail).toContainText("rb2610");
  await search.fill("unknown");
  await expect(board.locator(".market-search-empty")).toHaveText("没有匹配的市场合约");
  await expect(detail).toHaveCount(0);
  await search.fill("");
  await expect(detail).toContainText("i2701");
  await page.screenshot({ path: "build/market-board-desktop.png" });
  await page.setViewportSize({ width: 2048, height: 1152 });
  const first = await board.getByRole("region", { name: "上海商品期货" }).boundingBox();
  const second = await board.getByRole("region", { name: "大连商品期货" }).boundingBox();
  expect(second!.y).toBeGreaterThan(first!.y);
  await page.screenshot({ path: "build/market-board-wide.png" });
  await page.setViewportSize({ width: 800, height: 900 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await expect(detail).toBeVisible();
  await page.screenshot({ path: "build/market-board-compact.png" });
  await detail.getByRole("button", { name: "Tick", exact: true }).first().click();
  quoteCount = 24;
  const chart = detail.getByRole("img", { name: "最近报价走势" });
  await expect(chart).toBeVisible();
  await expect(chart).toHaveAttribute("data-visible-count", "24");
  await expect(detail.locator(".price-chart-cursor")).toContainText("100.00000001");
  await expect(chart.locator("rect")).toHaveCount(0);
  await detail.getByRole("button", { name: "放大", exact: true }).click();
  await expect(chart).toHaveAttribute("data-visible-count", "19");
  quoteCount = 30;
  await expect(chart).toHaveAttribute("data-window-start", "11");
  await detail.getByRole("button", { name: "向前", exact: true }).click();
  await expect(chart).toHaveAttribute("data-window-start", "7");
  quoteCount = 32;
  await expect(chart).toHaveAttribute("data-window-start", "7");
  await detail.getByRole("button", { name: "最新数据", exact: true }).click();
  await expect(chart).toHaveAttribute("data-window-start", "13");
  available = false;
  await expect(chart).toHaveCount(0);
  await expect(detail.getByText("报价序列暂不可用", { exact: true })).toBeVisible();
  interrupted = true;
  available = true;
  quoteCount = 3;
  await expect(chart).toHaveAttribute("data-visible-count", "3");
  await expect(
    detail.getByText("序列曾中断，仅显示恢复后的可用记录。", { exact: true }),
  ).toBeVisible();
  await board.getByRole("button", { name: "rb2610", exact: true }).click();
  await expect(chart).toHaveCount(0);
  await expect(detail.getByText("等待报价事件", { exact: true })).toBeVisible();
  await board.getByRole("button", { name: "i2701", exact: true }).click();
  await expect(chart).toBeVisible();
  await page.setViewportSize({ width: 1440, height: 900 });
  expect(
    await chart.evaluate(svg => {
      const right = svg.getBoundingClientRect().right;
      return [...svg.querySelectorAll("text")].every(
        text => text.getBoundingClientRect().right <= right + 1,
      );
    }),
  ).toBe(true);
  await page.screenshot({ path: "build/market-quote-trend.png" });
  // The whole quote row selects/opens its own contract, including numeric cells.
  const rbRow = board.locator("tbody tr").filter({ hasText: "rb2610" });
  await rbRow.locator("td").nth(1).click();
  await expect(detail).toContainText("rb2610");
  await expect(board.getByRole("button", { name: "rb2610", exact: true })).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(board.getByRole("button", { name: "i2701", exact: true })).toBeFocused();
  await expect(detail).toContainText("i2701");
  await rbRow.locator("td").nth(1).click();
  await page.screenshot({ path: "build/market-mouse-keyboard-selection.png" });
  await page.keyboard.press("Enter");
  await expect(page.locator('.contract-list-row[aria-pressed="true"]')).toContainText("rb2610");
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "市场", exact: true })
    .click();
  await board.getByRole("button", { name: "i2701", exact: true }).focus();
  await page.keyboard.press("Enter");
  await expect(page.locator('.contract-list-row[aria-pressed="true"]')).toContainText("i2701");
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "市场", exact: true })
    .click();
  await rbRow.locator("td").nth(1).dblclick();
  await expect(page.locator('.contract-list-row[aria-pressed="true"]')).toContainText("rb2610");
});
