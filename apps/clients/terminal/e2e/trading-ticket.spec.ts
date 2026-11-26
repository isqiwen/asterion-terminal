import { openView } from "./navigation-helper";
import { brokerReady, livePolicy, liveAccount, liveOrders, trading } from "./rpc-fixture";
import { test, expect } from "./trading-fixture";

// A position closes from its row through the ticket, and every working order
// is cancelled from the orders tab, on the test counter.
test(
  "a position closes from its row and working orders cancel together",
  { tag: ["@journey", "@trading"] },
  async ({ page, tradingHost }) => {
    test.setTimeout(40000);
    const account = tradingHost.connection;
    await trading(page.request, account, await livePolicy(page.request));
    await trading(page.request, account, { connectBroker: {} });
    await brokerReady(page.request, account);
    const opened = await liveAccount(page.request, account);
    const reviewed = { accountId: opened.id, policyRevision: opened.session.policyRevision };
    await trading(page.request, account, {
      sendCommand: { ...reviewed, requestId: "allow", authorize: { userId: "000001" } },
    });
    // Rest the larger buy before the fill requires reconciliation, below the later closing sell.
    for (const [id, lots, price] of [
      ["resting", "3", "3499"],
      ["filled", "1", "3500"],
    ])
      await trading(page.request, account, {
        sendCommand: {
          ...reviewed,
          requestId: id,
          submitOrder: {
            orderId: id,
            instrument: { exchange: "SHFE", symbol: "rb2610" },
            side: "SIDE_BUY",
            offset: "OFFSET_OPEN",
            lots: { text: lots },
            price: { text: price },
          },
        },
      });
    await page.goto("/");
    await openView(page, "trading");
    const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
    await page.setViewportSize({ width: 1280, height: 720 });
    await expect(panel.getByRole("button", { name: /^买入/ })).toBeInViewport({ ratio: 1 });
    await page.setViewportSize({ width: 1440, height: 900 });
    const position = panel.getByRole("table", { name: "CTP 持仓" }).locator("tbody tr");
    // 3500 × 1 lot × 10 from the counter's cost.
    await expect(position).toContainText("3500.00");
    await position.getByRole("button", { name: "平仓", exact: true }).click();
    const ticket = panel.getByRole("form", { name: "CTP 委托" });
    await expect(ticket.getByRole("textbox", { name: "委托手数" })).toHaveValue("1");
    await ticket.getByRole("textbox", { name: "限价" }).fill("3500");
    await ticket.getByRole("button", { name: /^卖出/ }).click();
    await expect(panel.getByRole("table", { name: "CTP 持仓" })).toHaveCount(0, { timeout: 15000 });

    await panel.getByRole("button", { name: "委托", exact: true }).click();
    await panel.getByRole("button", { name: "全部撤单", exact: true }).click();
    await panel.getByRole("button", { name: /^确认撤掉 1 笔/ }).click();
    await expect
      .poll(
        async () =>
          (await liveOrders(page.request, account)).find(
            (order: { id: string }) => order.id === "resting",
          )?.state,
        { timeout: 15000 },
      )
      .toBe("ORDER_STATE_CANCELLED");
    await page.screenshot({ path: "apps/clients/terminal/test-results/trading-ticket.png" });
  },
);
