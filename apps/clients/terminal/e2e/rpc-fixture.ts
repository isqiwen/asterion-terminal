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

// The account policy the trading page creates: rb2610 from the CTP catalog,
// 5 per order, 10 in total, 2 working orders, 2% from the latest price.
export async function livePolicy(request: APIRequestContext) {
  const units = (value: number) => ({ units: String(Math.round(value * 1e8)) });
  const catalog = (await rpc(request, "runtime.snapshot")).market.catalog;
  const row = catalog.contracts.find((item: { symbol: string }) => item.symbol === "rb2610");
  const [, product, deliveryMonth] = row.contract_id.split("/");
  return {
    liveCreate: {
      risk: { maxOrderQuantity: units(5), maxGrossQuantity: units(10), maxWorkingOrders: "2" },
      maxPriceDeviation: units(0.02),
      contracts: [
        {
          venue: row.venue,
          symbol: row.symbol,
          currency: "CNY",
          priceIncrement: units(Number(row.price_tick)),
          quantityIncrement: units(1),
          multiplier: units(row.multiplier),
          product,
          deliveryMonth,
        },
      ],
    },
  };
}

// The account as its Trading service reports it now.
export async function liveAccount(request: APIRequestContext, account: string) {
  return (await trading(request, account, { attach: {} })).live;
}
