import { createServer } from "node:net";
import { once } from "node:events";
import { openView } from "./navigation-helper";
import { brokerReady, livePolicy, liveAccount, rpc, trading } from "./rpc-fixture";
import { test, expect } from "./trading-fixture";

// The trading page does not wait for Market or Warehouse: with both silent,
// a resting order is still cancelled from it.
test(
  "a resting order is cancelled while Market and Warehouse do not answer",
  { tag: ["@trading"] },
  async ({ page, tradingHost }) => {
    test.setTimeout(90000);
    const account = tradingHost.connection;
    await trading(page.request, account, await livePolicy(page.request));
    await trading(page.request, account, { connectBroker: {} });
    await brokerReady(page.request, account);
    const opened = await liveAccount(page.request, account);
    const reviewed = { accountId: opened.id, policyRevision: opened.session.policyRevision };
    await trading(page.request, account, {
      sendCommand: { ...reviewed, requestId: "allow", authorize: { userId: "000001" } },
    });
    // More than two lots rest at the test counter.
    await trading(page.request, account, {
      sendCommand: {
        ...reviewed,
        requestId: "resting",
        submitOrder: {
          orderId: "resting",
          instrument: { exchange: "SHFE", symbol: "rb2610" },
          side: "SIDE_BUY",
          offset: "OFFSET_OPEN",
          lots: { text: "3" },
          price: { text: "3500" },
        },
      },
    });

    // A service that accepts connections and never answers.
    const silent = createServer(() => {});
    silent.listen(0, "127.0.0.1");
    await once(silent, "listening");
    const port = (silent.address() as { port: number }).port;
    const fingerprint = "0".repeat(64);
    await rpc(page.request, "market.endpoint.save", { host: "127.0.0.1", port, fingerprint });
    await rpc(page.request, "warehouse.endpoint.save", { host: "127.0.0.1", port, fingerprint });
    // Pages that asked Market just before are still waiting for it, each until
    // its call times out.
    const waiting = Promise.all(
      Array.from({ length: 6 }, () =>
        page.request
          .post("/__asterion/api", {
            data: { version: 1, method: "market.call", params: { request: { getBoard: {} } } },
          })
          .catch(() => undefined),
      ),
    );
    try {
      const started = Date.now();
      await page.goto("/");
      await openView(page, "trading");
      const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
      await panel
        .getByRole("navigation", { name: "CTP 账户" })
        .getByRole("button", { name: /^fixture/ })
        .click();
      await panel.getByRole("button", { name: "委托", exact: true }).click();
      await panel.getByRole("button", { name: "撤单", exact: true }).click();
      await expect
        .poll(
          async () =>
            (await liveAccount(page.request, account)).session.orders?.find(
              (order: { id: string }) => order.id === "resting",
            )?.state,
          { timeout: 15000 },
        )
        .toBe("ORDER_STATE_CANCELLED");
      // Nothing waited for the Market calls in flight.
      expect(Date.now() - started).toBeLessThan(15000);
    } finally {
      silent.close();
      await waiting;
    }
  },
);
