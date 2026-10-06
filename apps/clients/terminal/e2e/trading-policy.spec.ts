import { ctpConnection, rpc } from "./dataset-fixture";
import { checkSnapshot } from "./snapshot-schema";
import { test, expect } from "./test";

test("account policy changes retain the record, reject stale commands and recover without authorization", async ({
  page,
}) => {
  test.setTimeout(90000);
  const account = "policy-account";
  await ctpConnection(page.request, account, {
    broker_id: "9999",
    user_id: "000001",
    trade_front: "tcp://127.0.0.1:41205",
  });
  await rpc(page.request, "market.catalog", {
    account,
    password: "catalog-only",
    auth_code: "auth-code",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).market.catalog.phase)
    .toBe("ready");
  await rpc(page.request, "live.create", {
    account,
    max_order_quantity: "5",
    max_gross_quantity: "10",
    max_working_orders: "2",
    max_price_deviation: "0.02",
    contracts: [{ venue: "SHFE", symbol: "rb2610" }],
  });
  await rpc(page.request, "live.connect", {
    account,
    password: "test-password",
    auth_code: "auth-code",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).live[account].session.phase, {
      timeout: 20000,
    })
    .toBe("ready");
  const before = await rpc(page.request, "runtime.snapshot");
  const original = before.live[account].session!;
  const service = before.live[account].connection.session;
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  const panel = page.getByRole("region", { name: "CTP 交易账户", exact: true });
  await panel.getByRole("button", { name: "风控", exact: true }).click();
  const editor = panel.getByRole("region", { name: "账户政策", exact: true });
  await expect(editor).toContainText(original.policy_revision);
  await editor.getByLabel("总持仓量上限", { exact: true }).fill("8");
  await editor.getByLabel("我确认应用上述政策并重新登录", { exact: true }).check();
  await editor.getByRole("button", { name: "应用新政策", exact: true }).click();
  await expect(panel.getByTestId("live-phase")).toHaveText("未连接");
  const after = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("after policy change", after);
  const revised = after.live[account].session!;
  expect(revised.account_id).toBe(original.account_id);
  expect(revised.policy_revision).not.toBe(original.policy_revision);
  expect(revised.risk_artifact).toBe(original.risk_artifact);
  expect(revised.risk.max_gross_quantity).toBe("8");
  expect(revised.authorization).toBeNull();
  const stale = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "live.act",
      params: {
        account,
        account_id: original.account_id,
        policy_revision: original.policy_revision,
        action: "live_authorize",
        user_id: "000001",
        request_id: "stale-policy",
      },
    },
  });
  expect((await stale.json()).error.code).toBe("conflict");
  await rpc(page.request, "live.close", { account });
  await rpc(page.request, "node.action", { id: "local", service, action: "restart" });
  const recovered = await rpc(page.request, "live.open", { account });
  checkSnapshot("policy recovered", recovered);
  expect(recovered.live[account].session!.policy_revision).toBe(revised.policy_revision);
  expect(recovered.live[account].session!.risk.max_gross_quantity).toBe("8");
  expect(recovered.live[account].session!.authorization).toBeNull();
  await rpc(page.request, "live.close", { account });
});
