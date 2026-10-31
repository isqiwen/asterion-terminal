import { expect, type APIRequestContext } from "@playwright/test";

// One Native command through the development server; a reply with an error fails the test.
export async function rpc(
  request: APIRequestContext,
  method: string,
  params: Record<string, unknown> = {},
) {
  const response = await request.post("/__asterion/api", {
    data: { version: 1, method, params },
  });
  expect(response.ok()).toBe(true);
  const body = await response.json();
  expect(body.error, `${method} ${JSON.stringify(body.error)}`).toBeUndefined();
  return body.result;
}

// Counter details are entered once in Settings; specs create accounts through
// the same command. `market` makes the account the market data source, as the
// user would choose it.
export async function ctpConnection(
  request: APIRequestContext,
  id: string,
  fields: { broker_id: string; user_id: string; trade_front?: string; market?: boolean },
) {
  const { market, ...account } = fields;
  const existing = (await rpc(request, "runtime.snapshot")).ctp_connections ?? [];
  if (!existing.some((item: { id: string }) => item.id === id))
    await rpc(request, "ctp.connections.save", {
      id,
      name: id,
      revision: "",
      app_id: "client_app",
      trade_front: "tcp://127.0.0.1:1",
      market_front: "tcp://127.0.0.1:1",
      ...account,
    });
  if (market) await rpc(request, "ctp.connections.market", { id });
}
