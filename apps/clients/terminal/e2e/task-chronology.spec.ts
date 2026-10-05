import { rpc, seedDataset } from "./dataset-fixture";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";
import { join } from "node:path";

test("durable task order and dates survive reload independently of task IDs", async ({ page }) => {
  const suffix = crypto.randomUUID();
  const older = `z-first-${suffix}`,
    newer = `a-second-${suffix}`;
  const call = (method: string, params: Record<string, unknown> = {}) =>
    rpc(page.request, method, params);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  await seedDataset(
    page.request,
    Array.from({ length: 40 }, (_, i) => 100 + i + (i % 3)),
    "chronology",
  );
  const spec = { lookbacks: [2], horizon: 1, evaluation: { mode: "full_sample" } };
  await call("factor.submit", { ...spec, id: older });
  const submitted = await call("factor.submit", { ...spec, id: newer });
  const metadata = submitted.task_service.tasks.find((task: { id: string }) => task.id === newer);
  expect(metadata.submitted_at_ms).toBeGreaterThan(0);
  for (let reload = 0; reload < 2; reload++) {
    await page.reload();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "回测与因子", exact: true })
      .click();
    await page.getByRole("button", { name: "因子分析", exact: true }).click();
    const tasks = page.getByRole("region", { name: "回测与因子任务", exact: true });
    const ids = tasks.locator("tbody tr code");
    await expect(ids.first()).toHaveText(newer);
    await expect(ids.nth(1)).toHaveText(older);
    const latest = tasks.locator("tbody tr").first();
    await expect(latest.locator("time")).toHaveAttribute(
      "datetime",
      new Date(metadata.submitted_at_ms).toISOString(),
    );
    await latest.getByText("详情", { exact: true }).click();
    await expect(latest).toContainText("最近更新");
  }
  await expect
    .poll(async () => {
      const state = await call("runtime.snapshot");
      return state.task_service.tasks.find((task: { id: string }) => task.id === newer)?.state;
    })
    .toBe("succeeded");
  await call("task.page", { before_sequence: metadata.submission_sequence });
  const records = page.getByRole("region", { name: "回测与因子任务", exact: true });
  await expect(records.locator("tbody tr code").first()).toHaveText(older);
  await records.getByRole("button", { name: "最新记录", exact: true }).click();
  await expect(records.locator("tbody tr code").first()).toHaveText(newer);
  await page
    .locator(".workspace-tabs")
    .getByRole("button", { name: "回测与因子", exact: true })
    .click();
  await page.screenshot({ path: join(__dirname, "../test-results/task-chronology.png") });
  page = await openSettingsWindow(page);
  await page.getByLabel("语言", { exact: true }).selectOption("en-US");
  page = await closeSettingsWindow(page);
  await page
    .locator(".workspace-tabs")
    .getByRole("button", { name: "Backtest & Factors", exact: true })
    .click();
  await expect(page.getByRole("columnheader", { name: "Submitted", exact: true })).toBeVisible();
  await page.setViewportSize({ width: 800, height: 900 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(
    true,
  );
});
