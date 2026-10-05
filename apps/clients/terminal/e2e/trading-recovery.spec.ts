import { ctpConnection, rpc } from "./dataset-fixture";
import { checkSnapshot } from "./snapshot-schema";
import { test, expect } from "./test";

test("trading recovery retains account identity and does not restore authorization", async ({
  page,
}) => {
  test.setTimeout(90000);
  const account = "recovered-account";
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
  checkSnapshot("before trading restart", before);
  const oldRecord = before.live[account].session!.account_id;
  await rpc(page.request, "live.act", {
    account,
    account_id: oldRecord,
    policy_revision: before.live[account].session!.policy_revision,
    request_id: "authorization-before-restart",
    action: "live_authorize",
    user_id: "000001",
  });
  const service = before.live[account].connection.session;
  await rpc(page.request, "live.close", { account });
  await rpc(page.request, "node.action", { id: "local", service, action: "restart" });
  const restored = await rpc(page.request, "live.open", { account });
  checkSnapshot("restored trading account", restored);
  expect(restored.live[account].session!.account_id).toBe(oldRecord);
  expect(restored.live[account].session!.authorization).toBeNull();
  expect(restored.live[account].session!.policy_revision).toBe(
    before.live[account].session!.policy_revision,
  );
  const repeated = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "live.act",
      params: {
        account,
        account_id: oldRecord,
        policy_revision: before.live[account].session!.policy_revision,
        request_id: "authorization-before-restart",
        action: "live_authorize",
        user_id: "000001",
      },
    },
  });
  expect((await repeated.json()).error.code).toBe("conflict");
  await rpc(page.request, "live.close", { account });
});
