import { openView } from "./navigation-helper";
import { brokerReady, livePolicy, liveAccount, trading } from "./rpc-fixture";
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
    // More than two lots rest at the test counter; one fills at once. Opening
    // waits for positions to be reconciled after a fill, so the resting one goes first.
    for (const [id, lots] of [
      ["resting", "3"],
      ["filled", "1"],
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
            price: { text: "3500" },
          },
        },
      });
    await page.goto("/");
    await openView(page, "trading");
    const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
    // The confirmation's step goes once it is ticked.
    await panel.getByRole("checkbox", { name: "我已核对账户与柜台环境" }).click();
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
          (await liveAccount(page.request, account)).session.orders?.find(
            (order: { id: string }) => order.id === "resting",
          )?.state,
        { timeout: 15000 },
      )
      .toBe("ORDER_STATE_CANCELLED");
    await page.screenshot({ path: "apps/clients/terminal/test-results/trading-ticket.png" });
  },
);
