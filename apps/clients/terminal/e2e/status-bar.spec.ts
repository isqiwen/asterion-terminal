import { openView } from "./navigation-helper";
import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";
test("compact footer exposes services and navigation", async ({ page }) => {
  await page.goto("/");
  const service = page
    .locator(".status-bar")
    .getByRole("button", { name: "查看服务连接", exact: true });
  await expect(service).toBeVisible();
  await expect(page.getByRole("button", { name: "检查连接", exact: true })).toHaveCount(0);
  await service.click();
  const panel = page.getByRole("region", { name: "服务连接详情" });
  await expect(panel).toBeVisible();
  await expect(panel).not.toContainText("本机核心");
  await expect(panel).not.toContainText("本机服务");
  await expect(panel).toContainText("此电脑");
  await expect(panel).not.toContainText("Python 研究服务");
  // Lab is registered by the first research/history operation, not by opening
  // the workbench or inspecting its connections.
  await page.keyboard.press("Escape");
  await openView(page, "market");
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await service.click();
  await expect(panel).toContainText("Python 研究服务");
  await expect(panel.getByText("lab", { exact: true })).not.toBeVisible();
  await panel.getByText("连接详情", { exact: true }).click();
  await expect(panel.getByText("lab", { exact: true })).toBeVisible();
  await panel.getByText("连接详情", { exact: true }).click();
  await page.screenshot({ path: "apps/clients/terminal/test-results/status-bar-services.png" });
  await page.keyboard.press("Escape");
  await expect(panel).toHaveCount(0);
  await expect(service).toBeFocused();
  await service.click();
  await openView(page, "contract");
  await expect(panel).toHaveCount(0);
  await service.click();
  page = await openSettingsWindow(
    page,
    panel.getByRole("button", { name: "连接设置", exact: true }),
  );
  await expect(page.getByRole("heading", { name: "交易连接", exact: true })).toBeVisible();
});

test("revisioned polls accept unchanged replies from an idle core", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
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
    const service = page.getByRole("button", { name: "查看服务连接", exact: true });
    await expect(service).toHaveText("● 服务", { timeout: 60000 });
    enabled = true;
    await expect.poll(() => polls).toBe(1);
    // Hold a read across several live-state polling intervals.
    await page.waitForTimeout(1600);
    expect(polls).toBe(1);
    release();
    await expect.poll(() => polls).toBeGreaterThan(1);
    await expect(service).toHaveText("● 服务");
  } finally {
    release();
  }
});
