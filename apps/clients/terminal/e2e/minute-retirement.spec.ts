import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { rpc, seedHistory, openData } from "./dataset-fixture";

test("@journey minute payload cleanup is explicit, Data-owned and survives service restart", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101, 102], "retirement-ui", {
    product: "ag",
  });
  const id = seeded.source_dataset_ids[0];
  const snapshot = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    service: snapshot.research_service.service,
    action: "stop",
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  const archive = page.getByRole("region", { name: "数据仓库", exact: true });
  await archive
    .locator(`[data-dataset-id="${id}"]`)
    .getByRole("button", { name: "维护此版本", exact: true })
    .click();
  const maintenance = archive.getByRole("region", { name: "分钟版本维护", exact: true });
  await expect(maintenance).toContainText("无阻断项");
  await expect(maintenance.getByRole("button", { name: "删除此版本", exact: true })).toBeDisabled();
  await maintenance.getByRole("checkbox", { name: "我确认只清理上方列出的分钟版本" }).check();
  await maintenance.getByRole("button", { name: "删除此版本", exact: true }).click();
  await expect(maintenance).toContainText("载荷已删除，来源证据保留");
  await expect(archive.locator(`[data-dataset-id="${id}"]`)).toHaveCount(0);
  expect(
    (await rpc(page.request, "data.history.retirement.preview", { id })).minute_retirement.state,
  ).toBe("removed");
  await rpc(page.request, "node.action", {
    service: "historical-data",
    action: "stop",
  });
  await openData(page.request);
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).data?.online, {
      timeout: 10000,
    })
    .toBe(true);
  await archive.getByRole("button", { name: "查看封存与清理记录", exact: true }).click();
  const inventory = archive.getByRole("region", { name: "封存与清理记录", exact: true });
  await expect(inventory).toContainText(id);
  await expect(inventory).toContainText("载荷已删除，来源证据保留");
  await page.screenshot({ path: "build/minute-retirement-browser.png", fullPage: true });
});

test("draft use and a named dataset cannot be overridden by deletion confirmation", async ({
  page,
}) => {
  const seeded = await seedHistory(page.request, [100, 101, 102], "retirement-blocked", {
    product: "ni",
  });
  const id = seeded.source_dataset_ids[0];
  await rpc(page.request, "data.dataset.select", seeded);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  const archive = page.getByRole("region", { name: "数据仓库", exact: true });
  await archive
    .locator(`[data-dataset-id="${id}"]`)
    .getByRole("button", { name: "维护此版本", exact: true })
    .click();
  const maintenance = archive.getByRole("region", { name: "分钟版本维护", exact: true });
  await expect(maintenance).toContainText("本窗口草稿正在使用此版本，请先清除选择。");
  await expect(maintenance.getByRole("checkbox")).toHaveCount(0);
  await rpc(page.request, "data.dataset.save", { name: "retained-minute" });
  await rpc(page.request, "data.dataset.clear");
  await maintenance.getByRole("button", { name: "刷新维护预览", exact: true }).click();
  await expect(maintenance).toContainText("有关联记录，禁止删除");
  await expect(maintenance).toContainText("retained-minute");
  await expect(maintenance.getByRole("button", { name: "删除此版本", exact: true })).toHaveCount(0);
  expect(
    (await rpc(page.request, "data.history.retirement.execute", { id })).minute_retirement.state,
  ).toBe("blocked");
  const remaining = await rpc(page.request, "data.datasets", {
    venue: "",
    product: "ni",
    contract_id: "",
    source: "",
  });
  expect(remaining.history_datasets.map((item: { id: string }) => item.id)).toContain(id);
});
