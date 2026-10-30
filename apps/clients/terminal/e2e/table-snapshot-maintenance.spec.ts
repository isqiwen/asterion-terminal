import { test, expect } from "./test";
import { rpc, seedHistory, openData } from "./dataset-fixture";
import { openView } from "./navigation-helper";

test("snapshot maintenance preserves shared contents, requires confirmation and survives restart", async ({
  page,
}) => {
  const original = await seedHistory(page.request, [100, 101], "snapshot-original", {
    product: "ag",
  });
  const current = await seedHistory(page.request, [200, 201], "snapshot-current", {
    product: "ag",
    settlement: 200,
  });
  const id = original.daily.snapshot;
  expect(current.daily.snapshot).not.toBe(id);
  const head = (
    await rpc(page.request, "data.tables.retirement.preview", { id: current.daily.snapshot })
  ).table_retirement;
  expect(head.state).toBe("blocked");
  expect(head.current_head).toBe(true);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "tables");
  await page
    .locator("summary")
    .filter({ hasText: /^表快照维护$/ })
    .click();
  const panel = page.getByRole("region", { name: "表快照维护", exact: true });
  await panel.getByLabel("待维护快照 SHA-256").fill(id);
  await panel.getByRole("button", { name: "预览表快照维护", exact: true }).click();
  await expect(panel).toContainText("无阻断项，可退役快照");
  await expect(panel.getByRole("button", { name: "退役此表快照", exact: true })).toBeDisabled();
  await panel.getByRole("checkbox", { name: "确认仅退役上方快照及列出的独占对象" }).check();
  await panel.getByRole("button", { name: "退役此表快照", exact: true }).click();
  await expect(panel).toContainText("快照已退役，独占载荷已清理");
  expect((await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot).toBe(
    current.daily.snapshot,
  );
  const before = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    service: before.data.service,
    action: "stop",
  });
  await openData(page.request);
  const inventory = (await rpc(page.request, "data.tables.retirements", { after: "" }))
    .table_retirements.items;
  expect(inventory.find((item: { id: string }) => item.id === id).state).toBe("removed");
  const old = (await rpc(page.request, "data.tables.retirement.preview", { id })).table_retirement;
  expect(old.state).toBe("removed");
  expect(old.payload_bytes).toBe("0");
  expect((await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot).toBe(
    current.daily.snapshot,
  );
});

test("a named dataset blocks retirement of its fixed table snapshot", async ({ page }) => {
  const original = await seedHistory(page.request, [100, 101], "snapshot-held", { product: "ni" });
  await rpc(page.request, "data.dataset.select", original);
  await rpc(page.request, "data.dataset.save", { name: "retained-table-snapshot" });
  await rpc(page.request, "data.dataset.clear");
  await seedHistory(page.request, [200, 201], "snapshot-held-correction", {
    product: "ni",
    settlement: 200,
  });
  const id = original.daily.snapshot;
  const preview = (await rpc(page.request, "data.tables.retirement.preview", { id }))
    .table_retirement;
  const report = (
    await rpc(page.request, "data.tables.retirement.execute", {
      id,
      preview_digest: preview.preview_digest,
    })
  ).table_retirement;
  expect(report.state).toBe("blocked");
  expect(report.current_head).toBe(false);
  expect(
    report.references.some((ref: { name: string }) => ref.name === "retained-table-snapshot"),
  ).toBe(true);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "tables");
  await page
    .locator("summary")
    .filter({ hasText: /^表快照维护$/ })
    .click();
  const panel = page.getByRole("region", { name: "表快照维护", exact: true });
  await panel.getByLabel("待维护快照 SHA-256").fill(id);
  await panel.getByRole("button", { name: "预览表快照维护", exact: true }).click();
  await expect(panel).toContainText("retained-table-snapshot");
  await expect(panel).toContainText("快照仍被使用，禁止退役");
  await expect(panel.getByRole("checkbox")).toHaveCount(0);
});
