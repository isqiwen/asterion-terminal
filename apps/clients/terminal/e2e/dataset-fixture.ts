import { expect, type APIRequestContext } from "@playwright/test";
import { execFileSync } from "node:child_process";
import { resolve } from "node:path";
// Test-only history: minute versions and market-wide daily tables supplied by
// the compiled fixture into the isolated data warehouse. The daily table is
// where a dataset reads its settlement prices. This is not a client workflow;
// production history comes only from data-source plugins.
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

// Test setup names the actual owner explicitly; production never infers it from an id.
export async function taskRpc(
  request: APIRequestContext,
  kind: "data" | "research",
  method: string,
  params: Record<string, unknown> = {},
) {
  const snapshot = await rpc(request, "runtime.snapshot");
  const selected = kind === "data" ? snapshot.data : snapshot.research_service;
  expect(selected).toBeTruthy();
  return rpc(request, method, {
    ...params,
    owner: {
      kind,
      node_id: selected.node_id,
      service: selected.service,
      connection_id: selected.connection_id,
    },
  });
}

type Service = { id: string; directory: string };
type Snapshot = { nodes: { id: string; health: { services: Service[] } }[] };

// Returns the data.dataset.select parameters for the seeded minute versions
// and daily table snapshot of the SHFE `product` contract. `keep` adds to the
// current selection instead of starting a new one.
// A service answers business requests once its stores are open, after it reports online.
export async function openData(request: APIRequestContext) {
  await rpc(request, "node.data.local.open");
  await expect
    .poll(async () => (await rpc(request, "runtime.snapshot")).data?.online, { timeout: 10000 })
    .toBe(true);
  return rpc(request, "runtime.snapshot");
}

export async function openResearch(request: APIRequestContext) {
  await rpc(request, "node.research.local.open");
  await expect
    .poll(async () => (await rpc(request, "runtime.snapshot")).research_service?.online, {
      timeout: 10000,
    })
    .toBe(true);
  return rpc(request, "runtime.snapshot");
}

type ServiceHealth = { id: string; pid: number; state: string; health: string };
const localServices = async (request: APIRequestContext): Promise<ServiceHealth[]> =>
  (await rpc(request, "runtime.snapshot")).nodes.find((n: { id: string }) => n.id === "local")
    .health.services;

// Restarts local services through `restart` and returns once each runs as a new
// process that reports ready. A snapshot's `online` can still describe the
// previous process for a moment after it stopped.
export async function restartServices(
  request: APIRequestContext,
  services: string[],
  restart: () => Promise<unknown>,
) {
  const before = new Map((await localServices(request)).map(s => [s.id, s.pid]));
  await restart();
  await expect
    .poll(
      async () => {
        const current = await localServices(request);
        return services.every(id => {
          const s = current.find(item => item.id === id);
          return s && s.pid !== before.get(id) && s.state === "running" && s.health === "ready";
        });
      },
      { timeout: 20000 },
    )
    .toBe(true);
}

export async function seedHistory(
  request: APIRequestContext,
  prices: number[],
  id: string,
  {
    product = "rb",
    provider,
    keep = false,
    day = "2026-09-25",
    minuteDays = [],
    dailyDays = [],
    tablesOnly = false,
    dailyPrices = false,
    dailyVolume = 10000,
    shfeMargins = false,
    month = "2026-10",
    openInterest = 100,
    settlement = 110,
  }: {
    product?: string;
    provider?: string;
    keep?: boolean;
    day?: string;
    minuteDays?: string[];
    dailyDays?: string[];
    tablesOnly?: boolean;
    dailyPrices?: boolean;
    dailyVolume?: number;
    shfeMargins?: boolean;
    month?: string;
    openInterest?: number;
    settlement?: number;
  } = {},
) {
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  // The selection lives in the shared core process; earlier specs may leave one.
  if (!keep) await rpc(request, "data.dataset.clear");
  await openData(request);
  // The daily table is seeded under the sources the installed data source
  // offers: those are the ones the Terminal's pages read.
  const offered: { table: string; source: string; plugin_id: string }[] = (
    await rpc(request, "data.tables.coverage", {})
  ).table_coverage.sources;
  const dailySource = offered.find(
    item => item.table === "futures.daily" && (!provider || item.plugin_id === provider),
  )!;
  const of = (table: string) =>
    offered.find(item => item.table === table && item.plugin_id === dailySource.plugin_id)!;
  const tables = [
    "--daily-source",
    dailySource.source,
    "--calendar-source",
    of("futures.calendar").source,
    "--contracts-source",
    of("futures.contracts").source,
  ];
  const snapshot: Snapshot = await rpc(request, "node.action", {
    service: "historical-data",
    action: "stop",
  });
  const service = snapshot.nodes
    .find(node => node.id === "local")!
    .health.services.find(item => item.id === "historical-data")!;
  expect(resolve(service.directory)).toBe(
    resolve(process.env.ASTERION_ROOT!, "data/data/historical-data"),
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
      ...(tablesOnly ? ["--tables-only"] : []),
      ...(dailyPrices ? ["--daily-prices"] : []),
      "--daily-volume",
      String(dailyVolume),
      ...(shfeMargins ? ["--shfe-margins"] : []),
      "--month",
      month,
      "--open-interest",
      String(openInterest),
      "--settlement",
      String(settlement),
      ...tables,
    ],
    { encoding: "utf8" },
  );
  await openData(request);
  const { source_dataset_ids, daily, begin_day, end_day } = JSON.parse(output);
  const selection: {
    source_dataset_ids: string[];
    daily: {
      snapshot: string;
      daily_source: string;
      calendar_source: string;
      contracts_source: string;
    };
    begin_day: string;
    end_day: string;
  } = { source_dataset_ids, daily, begin_day, end_day };
  return selection;
}

export async function seedDataset(
  request: APIRequestContext,
  prices: number[],
  id: string,
  options: Parameters<typeof seedHistory>[3] = {},
) {
  const selection = await seedHistory(request, prices, id, options);
  await rpc(request, "node.research.local.open");
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
