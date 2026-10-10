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
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  const minute = archive.locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`);
  const daily = archive.locator(`[data-dataset-id="${seeded.settlement_dataset_ids[0]}"]`);
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
  await expect(usage).toContainText("未检查停止的远程数据服务");
  await page.screenshot({ path: "build/history-usage-browser.png", fullPage: true });
  await usage.getByRole("button", { name: "关闭", exact: true }).click();
  // A daily version can serve both market and settlement input. It must
  // appear once for the saved record, with both roles preserved.
  await rpc(page.request, "data.dataset.select", {
    ...seeded,
    source_dataset_ids: seeded.settlement_dataset_ids,
  });
  await rpc(page.request, "data.dataset.save", { name: "日线双用途" });
  await daily.getByRole("button", { name: "使用情况", exact: true }).click();
  const dual = table.getByRole("row").filter({ hasText: "日线双用途" });
  await expect(dual).toHaveCount(1);
  await expect(dual).toContainText("行情输入、结算输入");
  await expect(usage).toContainText("本窗口回测与因子草稿：行情输入、结算输入");
});

// Explicit UI fixture: real mTLS inventory and failures are covered by native tests.
test("other data services retain known references and disclose stopped or failed inspections", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101, 102], "remote-groups", {
    product: "zn",
  });
  await page.route("**/__asterion/api", async route => {
    if (route.request().postDataJSON().method !== "data.history.usage") {
      await route.continue();
      return;
    }
    const response = await route.fetch();
    const body = await response.json();
    body.result.history_usage.other_data_services = [
      {
        node: "测试节点 A",
        service: "backtest-factor-a",
        checked: true,
        references: [
          {
            kind: "saved_dataset",
            id: "saved",
            name: "跨服务输入",
            roles: ["market", "settlement"],
          },
        ],
      },
      {
        node: "测试节点 B",
        service: "backtest-factor-b",
        checked: false,
        references: [],
        error: "data/task service is not running; references were not inspected",
      },
      {
        node: "测试节点 C",
        service: "",
        checked: false,
        references: [],
        error: "historical service inspection timed out",
      },
    ];
    await route.fulfill({ response, json: body });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
  await archive
    .locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`)
    .getByRole("button", { name: "使用情况", exact: true })
    .click();
  const taskService = archive.getByRole("region", { name: "其他数据服务", exact: true });
  await expect(taskService.getByRole("table")).toContainText("跨服务输入");
  await expect(taskService.getByRole("table")).toContainText("行情输入、结算输入");
  await expect(taskService.getByRole("alert")).toHaveCount(2);
  const stopped = taskService.getByRole("region", {
    name: "测试节点 B / backtest-factor-b",
    exact: true,
  });
  await stopped.getByText("未完成检查的详情", { exact: true }).click();
  await expect(stopped).toContainText("数据或任务服务未运行，未检查引用");
  await page.setViewportSize({ width: 1024, height: 768 });
  await taskService.scrollIntoViewIfNeeded();
  await page.screenshot({
    path: "build/history-cross-backtest-factor-browser.png",
    fullPage: true,
  });
  expect(
    await page.locator(".terminal-business").evaluate(el => el.scrollWidth <= el.clientWidth + 1),
  ).toBe(true);
  await page.screenshot({ path: "build/history-remote-groups-browser.png", fullPage: true });
});

test("registered disconnected nodes are disclosed without reading identities or connecting", async ({
  page,
}) => {
  const { mkdir, writeFile, rm } = await import("node:fs/promises");
  const { join } = await import("node:path");
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const directory = join(
    process.env.ASTERION_NODE_DIRECTORY!,
    "enrollments",
    "offline-inspection-fixture",
  );
  const seeded = await seedHistory(page.request, [100, 101, 102], "offline-ui", { product: "ni" });
  await mkdir(directory, { recursive: true });
  await writeFile(join(directory, "enrollment.json"), "not parsed by the read-only name inventory");
  try {
    await page.goto("/");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
    const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
    await archive
      .locator(`[data-dataset-id="${seeded.source_dataset_ids[0]}"]`)
      .getByRole("button", { name: "使用情况", exact: true })
      .click();
    const offline = archive.getByRole("region", { name: "未连接的节点", exact: true });
    await expect(offline).toContainText("offline-inspection-fixture");
    await expect(offline).toContainText("本次未检查");
    await expect(offline.getByRole("alert")).toHaveCount(0);
    expect(
      (await rpc(page.request, "runtime.snapshot")).nodes.some(
        (node: { id: string }) => node.id === "offline-inspection-fixture",
      ),
    ).toBe(false);
    await offline.scrollIntoViewIfNeeded();
    await page.screenshot({ path: "build/history-offline-nodes-browser.png", fullPage: true });
  } finally {
    await rm(directory, { recursive: true });
  }
});
