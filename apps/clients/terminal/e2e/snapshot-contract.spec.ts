import { removeFolder } from "./cleanup";
import { test, expect, type APIRequestContext } from "./test";
import { checkSnapshot, snapshotValidator } from "./snapshot-schema";
import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

// The TypeScript types in the bridge are the single source of truth for the
// Terminal API. Real C++ responses are validated against a strict schema
// generated from them: a field the core sends but the UI does not declare, or a
// declared field the core omits, fails here instead of in production.
async function call(request: APIRequestContext, method: string, params: object = {}) {
  const response = await request.post("/__asterion/api", {
    data: { version: 1, method, params },
  });
  expect(response.ok()).toBe(true);
  const envelope = await response.json();
  expect(envelope.error, JSON.stringify(envelope.error)).toBeUndefined();
  return envelope.result;
}

test(
  "C++ startup, polls and market states conform to the Terminal API types",
  { tag: "@journey" },
  async ({ page }) => {
    test.setTimeout(90000);
    const unchanged = snapshotValidator("SnapshotUnchanged");
    const check: typeof checkSnapshot = checkSnapshot;
    const folder = await mkdtemp(join(tmpdir(), "asterion-contract-"));
    try {
      await page.goto("/");
      await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
        timeout: 60000,
      });
      const started = await call(page.request, "runtime.snapshot");
      check("after startup", started);

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
  },
);
