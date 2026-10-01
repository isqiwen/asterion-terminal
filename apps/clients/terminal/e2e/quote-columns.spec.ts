import { test, expect } from "@playwright/test";
import type { Snapshot } from "../src/bridge/client";

test("quote columns apply atomically, preserve values and navigation, and reset hidden sorting", async ({
  page,
}) => {
  await page.route("**/__asterion/api", async route => {
    const response = await route.fetch();
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
        watchlist: ["rb2610", "rb2701"].map(symbol => ({ venue: "SHFE", symbol })),
        subscriptions: ["rb2610", "rb2701"].map((symbol, index) => ({
          venue: "SHFE",
          symbol,
          state: "subscribed",
          error_code: 0,
          quote: {
            last: String(100 + index * 10),
            previous_settlement: "100",
            previous_close: null,
            open_interest_change: null,
            open: null,
            upper_limit: null,
            lower_limit: null,
            volume: 25 + index,
            bid: null,
            ask: null,
            high: null,
            low: null,
            bid_levels: Array.from({ length: 4 }, () => ({ price: null, quantity: null })),
            ask_levels: Array.from({ length: 4 }, () => ({ price: null, quantity: null })),
            bid_quantity: 0,
            ask_quantity: 0,
            trading_day: "20260929",
            action_day: "20260929",
            update_time: "10:00:00",
            open_interest: String(50 + index),
            received_ms: Date.now(),
            source_ms: Date.now(),
          },
        })),
      };
    }
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  const market = page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "市场", exact: true });
  await market.click();
  const board = page.locator(".futures-market-board");
  const menu = page.getByRole("dialog", { name: "行情设置" });
  const openMenu = async () => {
    if (!(await menu.count())) await page.getByRole("button", { name: "行情设置" }).click();
  };
  // Without a catalog, list the actual subscribed months.
  await openMenu();
  await menu.getByRole("button", { name: "全部月份", exact: true }).click();
  const heads = board.locator("thead .quote-heading-label");
  const original = ["名称", "现价", "涨跌幅(结)", "涨跌", "1分钟涨速", "持仓"];
  await expect(heads).toHaveText(original);
  const dialog = page.getByRole("dialog", { name: "行情表头设置" });
  const openColumns = async () => {
    await openMenu();
    await menu.getByRole("button", { name: "行情表头设置", exact: true }).click();
  };
  await openColumns();
  await dialog.getByRole("checkbox", { name: "持仓量", exact: true }).uncheck();
  await page.keyboard.press("Escape");
  await expect(dialog).toHaveCount(0);
  await expect(menu.getByRole("button", { name: "行情表头设置", exact: true })).toBeFocused();
  await expect(heads).toHaveText(original);
  await openColumns();
  await dialog.getByRole("checkbox", { name: "持仓量", exact: true }).uncheck();
  await dialog.getByRole("checkbox", { name: "1分钟涨速", exact: true }).uncheck();
  await dialog.getByRole("checkbox", { name: "成交量", exact: true }).check();
  await dialog.getByRole("button", { name: "应用列设置", exact: true }).click();
  await expect(heads).toHaveText(["名称", "现价", "涨跌幅(结)", "涨跌", "成交量"]);
  const row = board.locator("tbody tr").filter({ hasText: "rb2610" });
  await expect(row.locator("td").nth(1)).toHaveText("100");
  await expect(row.locator("td").nth(4)).toHaveText("25");
  await openColumns();
  await dialog
    .locator("li")
    .filter({ hasText: "成交量" })
    .dragTo(dialog.locator("li").filter({ hasText: "最新价" }));
  await dialog.getByRole("button", { name: "应用列设置", exact: true }).click();
  await expect(heads).toHaveText(["名称", "成交量", "现价", "涨跌幅(结)", "涨跌"]);
  await expect(row.locator("td").nth(1)).toHaveText("25");
  await expect(row.locator("td").nth(2)).toHaveText("100");
  await board
    .getByRole("columnheader")
    .getByRole("button", { name: "成交量", exact: true })
    .click();
  await expect(board.locator("tbody tr").first()).toContainText("rb2701");
  await openColumns();
  await dialog.getByRole("checkbox", { name: "成交量", exact: true }).uncheck();
  await dialog.getByRole("button", { name: "应用列设置", exact: true }).click();
  await expect(board.getByRole("columnheader", { name: "合约", exact: true })).toHaveAttribute(
    "aria-sort",
    "ascending",
  );
  await expect(board.locator("tbody tr").first()).toContainText("rb2610");
  await row.locator("td").nth(1).dblclick();
  await expect(page.locator('.contract-list-row[aria-pressed="true"]')).toContainText("rb2610");
  await market.click();
  await expect(heads).toHaveText(["名称", "现价", "涨跌幅(结)", "涨跌"]);
  await openColumns();
  for (const checkbox of await dialog.getByRole("checkbox").all()) await checkbox.uncheck();
  await dialog.getByRole("button", { name: "应用列设置", exact: true }).click();
  await expect(heads).toHaveText(["名称"]);
  await board.getByRole("button", { name: "rb2610", exact: true }).focus();
  await page.keyboard.press("ArrowDown");
  await expect(board.getByRole("button", { name: "rb2701", exact: true })).toBeFocused();
  await openColumns();
  await dialog.getByRole("button", { name: "恢复默认列", exact: true }).click();
  await dialog.getByRole("button", { name: "取消列设置", exact: true }).click();
  await expect(heads).toHaveText(["名称"]);
  await openColumns();
  await dialog.getByRole("button", { name: "恢复默认列", exact: true }).click();
  await dialog.getByRole("button", { name: "应用列设置", exact: true }).click();
  await expect(heads).toHaveText(original);
  await page.setViewportSize({ width: 640, height: 540 });
  await openColumns();
  expect(
    await dialog.evaluate(element => {
      const box = element.getBoundingClientRect();
      return box.left >= 0 && box.right <= innerWidth && box.top >= 0 && box.bottom <= innerHeight;
    }),
  ).toBe(true);
  await page.screenshot({ path: "build/quote-columns-compact.png" });
});
