import { liveAccount, livePolicy, marketState, trading } from "./rpc-fixture";
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
    await expect
      .poll(async () => (await liveAccount(page.request, account)).phase, { timeout: 20000 })
      .toBe("ready");
    const before = await liveAccount(page.request, account);
    const authorize = {
      accountId: before.accountId,
      policyRevision: before.policyRevision,
      command: { requestId: "authorization-before-restart", liveAuthorize: { userId: "000001" } },
    };
    await trading(page.request, account, authorize);
    await tradingHost.restart();
    const restored = await liveAccount(page.request, account);
    expect(restored.accountId).toBe(before.accountId);
    expect(restored.authorization).toBeUndefined();
    expect(restored.policyRevision).toBe(before.policyRevision);
    const repeated = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "trading.call", params: { account, request: authorize } },
    });
    expect((await repeated.json()).error.code).toBe("conflict");
  },
);
