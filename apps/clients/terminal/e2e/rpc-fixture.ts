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

// One trading.proto Request through the native relay; returns the Response as ProtoJSON.
export async function trading(
  request: APIRequestContext,
  account: string,
  body: Record<string, unknown>,
) {
  return (await rpc(request, "trading.call", { account, request: body })).trading;
}

// The saved Market's quotes and catalog as ProtoJSON, read through the native relay.
export async function marketState(request: APIRequestContext) {
  return (await rpc(request, "market.call", { request: { getBoard: {} } })).market.getBoard;
}

// The account policy the trading page creates: rb2610 from the CTP catalog,
// 5 per order, 10 in total, 2 working orders, 2% from the latest price.
export async function livePolicy(request: APIRequestContext) {
  const catalog = (await marketState(request)).catalog;
  const row = catalog.contracts.find(
    (item: { instrument: { symbol: string } }) => item.instrument.symbol === "rb2610",
  );
  const [, product, deliveryMonth] = row.contract.split("/");
  return {
    openAccount: {
      limits: {
        maxOrderLots: { text: "5" },
        maxGrossLots: { text: "10" },
        maxContractLots: { text: "10" },
        maxWorkingOrders: 2,
      },
      maxPriceDeviation: { text: "0.02" },
      contracts: [
        {
          instrument: row.instrument,
          product,
          deliveryMonth,
          multiplier: { text: String(row.multiplier) },
          priceTick: row.priceTick,
          lotSize: { text: "1" },
        },
      ],
    },
  };
}

// The account as its Trading service reports it now, with its open session.
export async function liveAccount(request: APIRequestContext, account: string) {
  return (await trading(request, account, { getAccount: {} })).getAccount;
}

// The account's strategy packages and instance.
export async function strategies(request: APIRequestContext, account: string) {
  return (await trading(request, account, { getStrategies: {} })).getStrategies;
}

// Polls until the account's broker session is ready.
export async function brokerReady(request: APIRequestContext, account: string) {
  await expect
    .poll(async () => (await liveAccount(request, account)).session?.brokerState, {
      timeout: 20000,
    })
    .toBe("BROKER_STATE_READY");
}
