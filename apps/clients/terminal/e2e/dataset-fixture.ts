import { expect, type APIRequestContext } from "@playwright/test";
import { execFileSync } from "node:child_process";
import { resolve } from "node:path";
// Test-only history: completed minute and daily download tasks written by the
// compiled test provider into the isolated node's research ledger. It is not a
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

// Returns the research.dataset.select parameters for the seeded downloads of
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
  }: {
    product?: string;
    keep?: boolean;
    day?: string;
    minuteDays?: string[];
    dailyDays?: string[];
  } = {},
) {
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  // The selection lives in the shared core process; earlier specs may leave one.
  if (!keep) await rpc(request, "research.dataset.clear");
  await rpc(request, "research.local");
  const snapshot: Snapshot = await rpc(request, "node.action", {
    id: "local",
    service: "research",
    action: "stop",
  });
  const service = snapshot.nodes
    .find(node => node.id === "local")!
    .health.services.find(item => item.id === "research")!;
  expect(resolve(service.directory)).toBe(
    resolve(process.env.ASTERION_NODE_DIRECTORY!, "services/research/ledger"),
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
    ],
    { encoding: "utf8" },
  );
  await rpc(request, "research.local");
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
  options: { product?: string; keep?: boolean; day?: string } = {},
) {
  const selection = await seedHistory(request, prices, id, options);
  return rpc(request, "research.dataset.select", selection);
}

// Counter details are entered once in Settings; specs create them through the
// same command and make the account current, as the user would.
export async function ctpConnection(
  request: APIRequestContext,
  id: string,
  fields: {
    broker_id: string;
    user_id: string;
    app_id?: string;
    trade_front?: string;
    market_front?: string;
  },
) {
  const existing = (await rpc(request, "runtime.snapshot")).ctp_connections ?? [];
  if (existing.some((item: { id: string }) => item.id === id))
    return rpc(request, "ctp.connections.select", { id });
  await rpc(request, "ctp.connections.save", {
    id,
    name: id,
    revision: "",
    app_id: "",
    trade_front: "",
    market_front: "",
    ...fields,
  });
  await rpc(request, "ctp.connections.select", { id });
}
