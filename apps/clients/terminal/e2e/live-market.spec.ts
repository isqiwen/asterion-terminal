import { openView } from "./navigation-helper";
import { marketState, rpc } from "./rpc-fixture";
import { test, expect } from "./test";

test(
  "remote Market survives client close and keeps desktop watchlists local",
  { tag: "@journey" },
  async ({ page }) => {
    await page.goto("/");
    await openView(page, "market");
    await page.getByRole("tab", { name: "实时行情", exact: true }).click();
    const panel = page.getByRole("region", { name: "实时期货行情" });
    await expect(panel.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
    const saved = async () => (await rpc(page.request, "runtime.snapshot")).market_endpoint;
    const watch = async () =>
      (await rpc(page.request, "market.watch", { restart: false })).market_watch;
    const status = async () =>
      (await rpc(page.request, "market.call", { request: { getStatus: {} } })).market.getStatus;
    const before = await status();
    expect((await saved()).instruments).toEqual([]);
    await expect(panel.locator('input[type="password"]')).toHaveCount(0);
    await panel.getByRole("button", { name: "加入自选", exact: true }).click();
    await page
      .getByRole("navigation", { name: "期货分类" })
      .getByRole("button", { name: "自选", exact: true })
      .click();
    await expect(panel.getByRole("button", { name: "rb2610", exact: true })).toBeVisible();
    await rpc(page.request, "market.close");
    expect(await watch()).toBeNull();
    await rpc(page.request, "market.open");
    expect((await status()).startedMs).toBe(before.startedMs);
    expect((await saved()).instruments).toEqual([{ exchange: "SHFE", symbol: "rb2610" }]);
    await rpc(page.request, "market.watchlist", { instruments: [] });
    // The saved watchlist is the desktop's own; Market still collects the contract.
    expect((await marketState(page.request)).rows).toHaveLength(1);
    await openView(page, "coverage");
    const state = page.getByRole("region", { name: "行情状态" });
    await expect(state.getByText("行情已连接", { exact: true })).toBeVisible();
    await expect(state.getByText("磁盘可用空间", { exact: true })).toBeVisible();
  },
);
