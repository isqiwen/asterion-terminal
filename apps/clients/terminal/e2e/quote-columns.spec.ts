import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { board as frameOf, serveWatch, watchRow } from "./market-fixture";

test("quote columns apply atomically, preserve values and navigation, and reset hidden sorting", async ({
  page,
}) => {
  // Two quoted months and no contract catalog, in one complete frame.
  const ids = ["rb2610", "rb2701"].map(symbol => ({ exchange: "SHFE", symbol }));
  let sent = false;
  await serveWatch(
    page,
    restart => {
      if (sent && !restart) return { frames: [], online: true };
      sent = true;
      const frame = frameOf(
        ids.map((id, index) =>
          watchRow(id, {
            last: String(100 + index * 10),
            previousSettlement: "100",
            volume: String(25 + index),
            openInterest: String(50 + index),
            receivedMs: String(Date.now()),
            exchangeMs: String(Date.now()),
          }),
        ),
      );
      return { frames: [{ full: frame }], online: true };
    },
    ids,
  );
  await page.goto("/");
  await openView(page, "market");
  const board = page.locator(".futures-market-board");
  // Without a catalog, list the actual subscribed months.
  await page.getByRole("button", { name: "全部月份", exact: true }).click();
  const heads = board.locator("thead .quote-heading-label");
  const original = ["名称", "现价", "涨跌幅(结)", "涨跌", "1分钟涨速", "持仓"];
  await expect(heads).toHaveText(original);
  const dialog = page.getByRole("dialog", { name: "行情表头设置" });
  const columnsButton = page.getByRole("button", { name: "表头设置", exact: true });
  const openColumns = () => columnsButton.click();
  await openColumns();
  await dialog.getByRole("checkbox", { name: "持仓量", exact: true }).uncheck();
  await page.keyboard.press("Escape");
  await expect(dialog).toHaveCount(0);
  await expect(columnsButton).toBeFocused();
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
  // Opening a row widens its chart beside the board.
  await row.locator("td").nth(1).dblclick();
  await expect(page.locator(".futures-market-board[data-expanded]")).toBeVisible();
  await expect(page.getByRole("complementary", { name: "合约详情" })).toContainText("rb2610");
  await page.getByRole("button", { name: "收起", exact: true }).click();
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
