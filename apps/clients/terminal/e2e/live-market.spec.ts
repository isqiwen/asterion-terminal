import { openView } from "./navigation-helper";
import { rpc } from "./rpc-fixture";
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
    const before = (await rpc(page.request, "runtime.snapshot")).market;
    expect(before.remote).toBe(true);
    expect(before.watchlist).toEqual([]);
    await expect(panel.locator('input[type="password"]')).toHaveCount(0);
    await panel.getByRole("button", { name: "加入自选", exact: true }).click();
    await openView(page, "watchlist");
    await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(1);
    await rpc(page.request, "market.close");
    expect((await rpc(page.request, "runtime.snapshot")).market).toBeNull();
    await rpc(page.request, "market.open");
    const after = (await rpc(page.request, "runtime.snapshot")).market;
    expect(after.instance_id).toBe(before.instance_id);
    expect(after.watchlist).toEqual([{ venue: "SHFE", symbol: "rb2610" }]);
    await rpc(page.request, "market.watchlist", { instruments: [] });
    expect((await rpc(page.request, "runtime.snapshot")).market.subscriptions).toHaveLength(1);
    await openView(page, "coverage");
    const recording = page.getByRole("region", { name: "Market 录制监控" });
    await expect(recording.getByText("行情已连接", { exact: true })).toBeVisible();
    await expect(recording.getByText("磁盘可用空间", { exact: true })).toBeVisible();
    expect((await rpc(page.request, "market.recording")).recording.instance).toBe(
      before.instance_id,
    );
  },
);
