import { removeFolder } from "./cleanup";
import { seedDataset } from "./dataset-fixture";
import { test, expect, type APIRequestContext } from "./test";
import { createGenerator } from "ts-json-schema-generator";
import Ajv from "ajv";
import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

// The TypeScript types in the bridge are the single source of truth for the
// Terminal API. Real C++ responses are validated against a strict schema
// generated from them: a field the core sends but the UI does not declare, or a
// declared field the core omits, fails here instead of in production.
const source = resolve(__dirname, "../src/bridge/client.ts");
function validator(type: "Snapshot" | "SnapshotUnchanged") {
  const schema = createGenerator({
    path: source,
    tsconfig: resolve(__dirname, "../tsconfig.json"),
    type,
    skipTypeCheck: true,
    additionalProperties: false,
  }).createSchema(type);
  return new Ajv({ allErrors: true, strict: false }).compile(schema);
}
async function call(request: APIRequestContext, method: string, params: object = {}) {
  const response = await request.post("/__asterion/api", {
    data: { version: 1, method, params },
  });
  expect(response.ok()).toBe(true);
  const envelope = await response.json();
  expect(envelope.error, JSON.stringify(envelope.error)).toBeUndefined();
  return envelope.result;
}

test("C++ snapshots conform to the Terminal API types in every state", async ({ page }) => {
  test.setTimeout(90000);
  const snapshot = validator("Snapshot");
  const unchanged = validator("SnapshotUnchanged");
  const check = (label: string, value: unknown) => {
    const valid = snapshot(value);
    expect(valid, `${label}: ${JSON.stringify(snapshot.errors, null, 1)}`).toBe(true);
  };
  const folder = await mkdtemp(join(tmpdir(), "asterion-contract-"));
  try {
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
      timeout: 60000,
    });
    const started = await call(page.request, "runtime.snapshot");
    check("after startup", started);

    check("after dataset selection", await seedDataset(page.request, [100, 99], "contract"));

    // Poll replies: either a full snapshot or an explicit unchanged marker.
    let revision = started.revision as number;
    for (let attempt = 0; attempt < 5; ++attempt) {
      const polled = await call(page.request, "runtime.snapshot", { since: revision });
      if ("unchanged" in polled) {
        expect(unchanged(polled), JSON.stringify(unchanged.errors)).toBe(true);
        break;
      }
      check("poll", polled);
      revision = polled.revision;
      await page.waitForTimeout(2500);
    }
    // Research: a queued, running and finished backtest, then its result.
    await call(page.request, "research.submit", {
      id: "contract-backtest",
      fast: 1,
      slow: 2,
      quantity: "1",
      deposit: "10000",
      contracts: [
        {
          venue: "SHFE",
          symbol: "rb2610",
          cost_schedule: [
            {
              effective_from: "1970-01-01",
              source: "test fixture",
              values: {
                margin_per_lot: "100",
                open_fee: "2",
                close_today_fee: "3",
                close_yesterday_fee: "4",
                margin_rate: "0",
                open_fee_rate: "0",
                close_today_fee_rate: "0",
                close_yesterday_fee_rate: "0",
              },
            },
          ],
        },
      ],
      max_order_quantity: "10",
      max_gross_quantity: "10",
      max_working_orders: "10",
    });
    await expect
      .poll(
        async () => {
          const state = await call(page.request, "runtime.snapshot");
          check("research in progress", state);
          return state.research?.tasks.find(
            (task: { id: string }) => task.id === "contract-backtest",
          )?.state;
        },
        { timeout: 30000 },
      )
      .toBe("succeeded");
    check(
      "research result",
      await call(page.request, "research.result", { id: "contract-backtest" }),
    );

    // Live market through the test SDK (ASTERION_CTP_LIBRARY from the e2e wrapper).
    await call(page.request, "ctp.connections.save", {
      id: "fixture",
      name: "Fixture",
      revision: "",
      broker_id: "test",
      user_id: "fixture",
      app_id: "app",
      trade_front: "tcp://127.0.0.1:1",
      market_front: "tcp://127.0.0.1:1",
    });
    const connected = await call(page.request, "market.connect", {
      password: "contract-only",
      instruments: [{ venue: "SHFE", symbol: "rb2610" }],
    });
    check("market connected", connected);
    check("market disconnected", await call(page.request, "market.disconnect"));
  } finally {
    await removeFolder(folder);
  }
});
