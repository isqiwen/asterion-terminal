import { openView } from "./navigation-helper";
import { test, expect } from "./test";
test("each service has its own indicator; Lab appears once it is used", async ({ page }) => {
  await page.goto("/");
  const bar = page.locator(".status-bar");
  await expect(bar.getByText(/^● 行情/)).toBeVisible({ timeout: 60000 });
  await expect(bar.getByText(/^● 研究/)).toHaveCount(0);
  await openView(page, "market");
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await expect(bar.getByText("● 研究", { exact: true })).toBeVisible({ timeout: 60000 });
  await page.screenshot({ path: "apps/clients/terminal/test-results/status-bar-services.png" });
});

test("revisioned polls accept unchanged replies from an idle core", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".status-bar .status-item").first()).toBeVisible({
    timeout: 60000,
  });
  // Exercise the real bridge module in the page, not a copy of its logic.
  const result = await page.evaluate(async () => {
    // Served by the Vite dev server; typed from the same source file.
    const path = "/src/bridge/client.ts";
    const client = (await import(/* @vite-ignore */ path)) as typeof import("../src/bridge/client");
    // A background refresh may publish once between the two calls while
    // services settle; an idle core then answers with the same revision.
    let first = await client.request("runtime.snapshot");
    let poll = await client.pollSnapshot(first.revision);
    for (let attempt = 0; attempt < 5 && !("unchanged" in poll); ++attempt) {
      await new Promise(resolve => setTimeout(resolve, 500));
      first = await client.request("runtime.snapshot");
      poll = await client.pollSnapshot(first.revision);
    }
    return { revision: first.revision, poll };
  });
  expect(result.poll).toMatchObject({ unchanged: true, revision: result.revision });
});

test("a slow status read does not accumulate overlapping polls", async ({ page }) => {
  let enabled = false,
    polls = 0;
  let release!: () => void;
  const held = new Promise<void>(resolve => {
    release = resolve;
  });
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!enabled || request.method !== "runtime.snapshot" || !("since" in request.params))
      return route.continue();
    const index = polls++;
    const response = await route.fetch();
    if (index === 0) await held;
    await route.fulfill({ response });
  });
  try {
    await page.goto("/");
    const market = page.locator(".status-bar").getByText(/^● 行情/);
    await expect(market).toBeVisible({ timeout: 60000 });
    enabled = true;
    await expect.poll(() => polls).toBe(1);
    // Hold a read across several live-state polling intervals.
    await page.waitForTimeout(1600);
    expect(polls).toBe(1);
    release();
    await expect.poll(() => polls).toBeGreaterThan(1);
    await expect(page.locator(".status-updated")).toHaveCount(0);
  } finally {
    release();
  }
});
