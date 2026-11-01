import { test, expect } from "./test";
import { rpc } from "./rpc-fixture";

test("Market address replaces desktop counter credentials", async ({ page }) => {
  await page.goto("/?settings=sources");
  // The command contract accepts a service address and never a broker password.
  const state = await rpc(page.request, "runtime.snapshot");
  expect(state.market_endpoint.host).toBe("127.0.0.1");
  expect(state.ctp_connections).toBeUndefined();
  expect(
    state.nodes
      .flatMap((n: { health?: { services: { kind: string }[] } }) => n.health?.services ?? [])
      .every((s: { kind: string }) => s.kind === "lab"),
  ).toBe(true);
});
