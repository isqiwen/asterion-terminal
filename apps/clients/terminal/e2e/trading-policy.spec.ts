import { openView } from "./navigation-helper";
import { brokerReady, liveAccount, livePolicy, marketState, trading } from "./rpc-fixture";
import { test, expect } from "./trading-fixture";

test(
  "account policy changes retain the record, reject stale commands and recover without authorization",
  { tag: "@trading" },
  async ({ page, tradingHost }) => {
    test.setTimeout(90000);
    const account = tradingHost.connection;
    await expect
      .poll(async () => (await marketState(page.request)).catalog?.status?.state)
      .toBe("CATALOG_STATE_READY");
    await trading(page.request, account, await livePolicy(page.request));
    await brokerReady(page.request, account);
    const original = await liveAccount(page.request, account);
    await page.goto("/");
    await openView(page, "trading");
    const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
    await panel.getByRole("button", { name: "风控", exact: true }).click();
    const editor = panel.getByRole("region", { name: "账户政策", exact: true });
    await expect(editor).toContainText(original.session.policyRevision);
    await editor.getByLabel("总持仓量上限", { exact: true }).fill("8");
    await editor.getByLabel("我确认应用上述政策并重新登录", { exact: true }).check();
    await editor.getByRole("button", { name: "应用新政策", exact: true }).click();
    await expect(panel.getByTestId("live-phase")).toHaveText("未连接");
    const revised = await liveAccount(page.request, account);
    expect(revised.id).toBe(original.id);
    expect(revised.session.policyRevision).not.toBe(original.session.policyRevision);
    expect(revised.session.policy.limits.maxGrossLots.text).toBe("8");
    expect(revised.session.authorizedMs).toBeUndefined();
    const stale = await page.request.post("/__asterion/api", {
      data: {
        version: 1,
        method: "trading.call",
        params: {
          account,
          request: {
            sendCommand: {
              requestId: "stale-policy",
              accountId: original.id,
              policyRevision: original.session.policyRevision,
              authorize: { userId: "000001" },
            },
          },
        },
      },
    });
    expect((await stale.json()).error.code).toBe("conflict");
    await tradingHost.restart();
    const recovered = await liveAccount(page.request, account);
    expect(recovered.session.policyRevision).toBe(revised.session.policyRevision);
    expect(recovered.session.policy.limits.maxGrossLots.text).toBe("8");
    expect(recovered.session.authorizedMs).toBeUndefined();
  },
);
