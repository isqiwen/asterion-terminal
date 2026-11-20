import { brokerReady, liveAccount, livePolicy, marketState, trading } from "./rpc-fixture";
import { test, expect } from "./trading-fixture";

test(
  "trading recovery retains account identity and does not restore authorization",
  { tag: ["@journey", "@trading"] },
  async ({ page, tradingHost }) => {
    test.setTimeout(90000);
    const account = tradingHost.connection;
    await expect
      .poll(async () => (await marketState(page.request)).catalog?.status?.state)
      .toBe("CATALOG_STATE_READY");
    await trading(page.request, account, await livePolicy(page.request));
    await brokerReady(page.request, account);
    const before = await liveAccount(page.request, account);
    const authorize = {
      sendCommand: {
        requestId: "authorization-before-restart",
        accountId: before.id,
        policyRevision: before.session.policyRevision,
        authorize: { userId: "000001" },
      },
    };
    await trading(page.request, account, authorize);
    await tradingHost.restart();
    const restored = await liveAccount(page.request, account);
    expect(restored.id).toBe(before.id);
    expect(restored.session.authorizedMs).toBeUndefined();
    expect(restored.session.policyRevision).toBe(before.session.policyRevision);
    const repeated = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "trading.call", params: { account, request: authorize } },
    });
    expect((await repeated.json()).error.code).toBe("conflict");
  },
);
