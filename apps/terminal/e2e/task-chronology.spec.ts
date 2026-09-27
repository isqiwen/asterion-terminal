import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("durable task order and dates survive reload independently of task IDs", async ({ page }) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-task-order-"));
  const csv = join(folder, "chronology.csv");
  const suffix = crypto.randomUUID();
  const older = `z-first-${suffix}`,
    newer = `a-second-${suffix}`;
  const olderData = `z-data-${suffix}`,
    newerData = `a-data-${suffix}`;
  const call = async (method: string, params: Record<string, unknown> = {}) => {
    const response = await page.request.post("/__asterion/api", {
      data: { version: 1, method, params },
    });
    const envelope = await response.json();
    expect(envelope.error).toBeUndefined();
    return envelope.result;
  };
  try {
    await writeFile(
      csv,
      "timestamp_ns,price,quantity\n" +
        Array.from(
          { length: 40 },
          (_, i) => `${1790298000000000000n + BigInt(i) * 1000000000n},${100 + i + (i % 3)},1`,
        ).join("\n") +
        "\n",
    );
    await page.goto("/");
    await call("research.local");
    await call("futures.inspect_csv", {
      path: csv,
      venue: "SHFE",
      symbol: "rb2610",
      product: "rb",
      delivery_month: "2026-10",
      currency: "CNY",
      price_increment: "1",
      quantity_increment: "1",
      multiplier: "10",
    });
    const spec = { lookbacks: [2], horizon: 1, evaluation: { mode: "full_sample" } };
    await call("research.factor.submit", { ...spec, id: older });
    const submitted = await call("research.factor.submit", { ...spec, id: newer });
    const metadata = submitted.research.tasks.find((task: { id: string }) => task.id === newer);
    expect(metadata.submitted_at_ms).toBeGreaterThan(0);
    await call("research.data.submit", { id: olderData });
    await call("research.data.submit", { id: newerData });
    for (let reload = 0; reload < 2; reload++) {
      await page.reload();
      await page.getByRole("button", { name: "研究", exact: true }).click();
      const tasks = page.getByRole("region", { name: "研究任务", exact: true });
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
      await page.getByRole("button", { name: "数据", exact: true }).click();
      const publications = page.getByRole("region", { name: "数据发布", exact: true });
      await expect(publications.locator("article code").first()).toHaveText(newerData);
      await expect(publications.locator("article code").nth(1)).toHaveText(olderData);
      await expect(
        publications.locator("article").first().getByLabel("提交时间", { exact: true }),
      ).toBeVisible();
    }
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await page.screenshot({ path: join(__dirname, "../test-results/task-chronology.png") });
    page = await openSettingsWindow(page);
    await page.getByLabel("语言", { exact: true }).selectOption("en-US");
    page = await closeSettingsWindow(page);
    await page.getByRole("button", { name: "Research", exact: true }).click();
    await expect(page.getByRole("columnheader", { name: "Submitted", exact: true })).toBeVisible();
    await page.setViewportSize({ width: 800, height: 900 });
    expect(
      await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth),
    ).toBe(true);
  } finally {
    await rm(folder, { recursive: true, force: true });
  }
});
