import { expect, type APIRequestContext } from "@playwright/test";
import { execFileSync } from "node:child_process";
import { resolve } from "node:path";
// Test-only history: verified minute and daily versions published by the
// compiled test provider into the isolated data warehouse. It is not a
// client workflow; production history comes only from data-source plugins.
const build = process.env.ASTERION_CPP_BUILD ?? resolve(__dirname, "../../../../build/Debug");

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

type Service = { id: string; directory: string };
type Snapshot = { nodes: { id: string; health: { services: Service[] } }[] };

// Returns the data.dataset.select parameters for the seeded downloads of
// the SHFE `product` 2026-10 contract. `keep` adds to the current selection
// instead of starting a new one.
export async function seedHistory(
  request: APIRequestContext,
  prices: number[],
  id: string,
  {
    product = "rb",
    keep = false,
    day = "2026-09-25",
    minuteDays = [],
    dailyDays = [],
    month = "2026-10",
    openInterest = 100,
    settlement = 110,
  }: {
    product?: string;
    keep?: boolean;
    day?: string;
    minuteDays?: string[];
    dailyDays?: string[];
    month?: string;
    openInterest?: number;
    settlement?: number;
  } = {},
) {
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  // The selection lives in the shared core process; earlier specs may leave one.
  if (!keep) await rpc(request, "data.dataset.clear");
  await rpc(request, "node.data_tasks.local.open");
  const snapshot: Snapshot = await rpc(request, "node.action", {
    id: "local",
    service: "historical-data",
    action: "stop",
  });
  const service = snapshot.nodes
    .find(node => node.id === "local")!
    .health.services.find(item => item.id === "historical-data")!;
  expect(resolve(service.directory)).toBe(
    resolve(process.env.ASTERION_NODE_DIRECTORY!, "services/historical-data/ledger"),
  );
  const output = execFileSync(
    resolve(build, "asterion_test_history"),
    [
      "--directory",
      service.directory,
      "--id",
      id,
      "--product",
      product,
      "--day",
      day,
      "--price",
      ...prices.map(String),
      ...(minuteDays.length ? ["--minute-days", ...minuteDays] : []),
      ...(dailyDays.length ? ["--daily-days", ...dailyDays] : []),
      "--month",
      month,
      "--open-interest",
      String(openInterest),
      "--settlement",
      String(settlement),
    ],
    { encoding: "utf8" },
  );
  await rpc(request, "node.data_tasks.local.open");
  await expect
    .poll(async () => (await rpc(request, "runtime.snapshot")).data?.online, { timeout: 10000 })
    .toBe(true);
  return JSON.parse(output) as {
    source_dataset_ids: string[];
    settlement_dataset_ids: string[];
    begin_day: string;
    end_day: string;
    price_increment: string;
    multiplier: string;
  };
}

export async function seedDataset(
  request: APIRequestContext,
  prices: number[],
  id: string,
  options: Parameters<typeof seedHistory>[3] = {},
) {
  const selection = await seedHistory(request, prices, id, options);
  return rpc(request, "data.dataset.select", selection);
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
