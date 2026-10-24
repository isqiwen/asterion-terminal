import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { seedHistory, rpc } from "./dataset-fixture";

test("usage shows fixed source and saved references and refreshes the draft", async ({ page }) => {
  const seeded = await seedHistory(page.request, [101, 102, 103, 104, 105, 106, 107], "usage-ui", {
    product: "al",
  });
  await rpc(page.request, "data.dataset.select", seeded);
  await rpc(page.request, "data.dataset.save", { name: "引用核对组合" });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  const minute = archive.locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`);
  // The daily table is not a second archive version.
  await expect(archive.locator("[data-dataset-id]")).toHaveCount(1);
  await minute.getByRole("button", { name: "使用情况", exact: true }).click();
  const usage = archive.getByRole("region", { name: "使用情况", exact: true });
  // Published fixture versions have no fabricated download tasks.
  await expect(usage).toContainText("找到 1 条关联记录");
  await expect(usage).toContainText("本窗口回测与因子草稿：行情输入");
  const table = usage.getByRole("table", { name: "数据版本关联记录" });
  await expect(table.getByRole("row").filter({ hasText: "引用核对组合" })).toContainText(
    "行情输入",
  );
  await rpc(page.request, "data.dataset.clear");
  await usage.getByRole("button", { name: "刷新使用情况", exact: true }).click();
  await expect(usage).not.toContainText("本窗口回测与因子草稿");
  await usage.getByText("检查范围与保留说明", { exact: true }).click();
  await expect(usage).toContainText("不通过进程清单推断引用");
  await page.screenshot({ path: "build/history-usage-browser.png", fullPage: true });
  await usage.getByRole("button", { name: "关闭", exact: true }).click();
});

// UI-only reply shaping: durable retention and release are covered by real service tests.
test("owner ledger retains offline consumers and an unavailable authority is never an all-clear", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101, 102], "owner-ledger-ui", {
    product: "zn",
  });
  let unavailable = false;
  await page.route("**/__asterion/api", async route => {
    if (route.request().postDataJSON().method !== "data.history.usage") {
      await route.continue();
      return;
    }
    const response = await route.fetch();
    const body = await response.json();
    if (unavailable) {
      delete body.result;
      body.error = { code: "unavailable", message: "retention authority unavailable" };
    } else {
      body.result.history_usage.consumers = ["prepared", "confirmed"].map((phase, index) => ({
        node: "offline-consumer-node",
        service: "stopped-research",
        domain: "research",
        kind: "backtest",
        id: `retained-run-${index}`,
        version: "a".repeat(64),
        phase,
      }));
    }
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  await archive
    .locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`)
    .getByRole("button", { name: "使用情况", exact: true })
    .click();
  const usage = archive.getByRole("region", { name: "使用情况", exact: true });
  const references = usage.getByRole("table", { name: "跨域保留引用", exact: true });
  await expect(references).toContainText("准备中的引用仍保留");
  await expect(references).toContainText("正式引用仍保留");
  await expect(references).toContainText("offline-consumer-node");
  await page.setViewportSize({ width: 1024, height: 768 });
  expect(
    await page.locator(".terminal-business").evaluate(el => el.scrollWidth <= el.clientWidth + 1),
  ).toBe(true);
  unavailable = true;
  await usage.getByRole("button", { name: "刷新使用情况", exact: true }).click();
  await expect(usage.getByRole("alert")).toBeVisible();
  await expect(usage.getByRole("table")).toHaveCount(0);
  await expect(usage).not.toContainText("未登记保留引用");
});

test("unconnected node registrations do not replace Data reference authority or trigger connection", async ({
  page,
}) => {
  const { mkdir, writeFile, readFile, rm } = await import("node:fs/promises");
  const { join } = await import("node:path");
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const directory = join(
    process.env.ASTERION_NODE_DIRECTORY!,
    "enrollments",
    "offline-inspection-fixture",
  );
  const seeded = await seedHistory(page.request, [100, 101, 102], "offline-ui", { product: "ni" });
  await mkdir(directory, { recursive: true });
  const marker = "synthetic registration must remain unread and unchanged";
  await writeFile(join(directory, "enrollment.json"), marker);
  try {
    await page.goto("/");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await openView(page, "records");
    const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
    await archive
      .locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`)
      .getByRole("button", { name: "使用情况", exact: true })
      .click();
    const usage = archive.getByRole("region", { name: "使用情况", exact: true });
    await expect(usage).toContainText("当前拥有者未登记保留引用");
    await expect(usage.getByRole("alert")).toHaveCount(0);
    await usage.getByText("检查范围与保留说明", { exact: true }).click();
    await expect(usage).toContainText("不通过进程清单推断引用");
    expect(await readFile(join(directory, "enrollment.json"), "utf8")).toBe(marker);
    expect(
      (await rpc(page.request, "runtime.snapshot")).nodes.some(
        (node: { id: string }) => node.id === "offline-inspection-fixture",
      ),
    ).toBe(false);
  } finally {
    await rm(directory, { recursive: true });
  }
});
