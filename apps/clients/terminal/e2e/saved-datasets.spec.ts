import { readFile, writeFile } from "node:fs/promises";
import { resolve, join, sep } from "node:path";
import { test, expect } from "./test";
import { seedHistory, rpc } from "./dataset-fixture";

test("named portfolio survives service restart and restores exact inputs atomically", async ({
  page,
}) => {
  await page.goto("/");
  const one = await seedHistory(page.request, [100, 102, 104], "saved-rb");
  const two = await seedHistory(page.request, [200, 205, 210], "saved-cu", {
    product: "cu",
    keep: true,
  });
  await rpc(page.request, "research.dataset.select", {
    ...one,
    price_increment: "2",
    multiplier: "10",
  });
  const seeded = await rpc(page.request, "research.dataset.select", {
    ...two,
    price_increment: "5",
    multiplier: "5",
  });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await page.getByRole("button", { name: "新建回测", exact: true }).click();
  await page.getByText("保存当前选择", { exact: true }).click();
  await page.getByLabel("数据集名称", { exact: true }).fill("金属组合");
  await page.getByLabel("数据集名称", { exact: true }).press("Enter");
  await expect(
    page.getByText("数据集已保存，可在当前数据服务中重复使用。", { exact: true }),
  ).toBeVisible();
  await rpc(page.request, "research.dataset.save", { name: "金属组合" });
  const library = (await rpc(page.request, "research.dataset.saved")).saved_datasets;
  expect(library.filter((item: { name: string }) => item.name === "金属组合")).toHaveLength(1);
  const original = library.find((item: { name: string }) => item.name === "金属组合");
  expect(original.selections).toHaveLength(2);
  expect(
    original.selections.map((item: { source_dataset_ids: string[] }) => item.source_dataset_ids),
  ).toEqual([one.source_dataset_ids, two.source_dataset_ids]);
  // A same-name save with changed contract units creates another fixed revision.
  await rpc(page.request, "research.dataset.select", { ...one, multiplier: "20" });
  await rpc(page.request, "research.dataset.save", { name: "金属组合" });
  expect(
    (await rpc(page.request, "research.dataset.saved")).saved_datasets.filter(
      (item: { name: string }) => item.name === "金属组合",
    ),
  ).toHaveLength(2);
  await rpc(page.request, "research.dataset.clear");
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await rpc(page.request, "node.action", { id: "local", service: "research", action: "stop" });
  await rpc(page.request, "research.local");
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await page.getByRole("button", { name: "新建回测", exact: true }).click();
  await page.getByRole("combobox", { name: "已保存数据集", exact: true }).selectOption(original.id);
  await page.getByRole("button", { name: "使用已保存数据集", exact: true }).click();
  await expect(page.getByRole("list", { name: "已选合约" }).locator("li")).toHaveCount(2);
  const restored = (await rpc(page.request, "runtime.snapshot")).datasets;
  expect(restored).toEqual(seeded.datasets);
  await expect(page.getByRole("button", { name: "下一步", exact: true })).toBeEnabled();
  await page.screenshot({
    path: "build/audit-review/named-datasets/selection.png",
    fullPage: true,
  });
  // Failed restore must retain the entire current selection.
  const response = await page.request.post("/__asterion/api", {
    data: { version: 1, method: "research.dataset.use", params: { id: "a".repeat(64) } },
  });
  expect((await response.json()).error).toBeTruthy();
  expect((await rpc(page.request, "runtime.snapshot")).datasets).toEqual(restored);
  // The first contract can resolve successfully while the second has corrupt storage.
  // Nothing from that partial restore may replace a newer current selection.
  const current = await rpc(page.request, "research.dataset.select", { ...one, multiplier: "30" });
  const evidence = await rpc(page.request, "research.result", { id: "saved-cu-bars" });
  const directory = evidence.research_result.result.directory as string;
  expect(resolve(directory).startsWith(resolve(process.env.ASTERION_NODE_DIRECTORY!) + sep)).toBe(
    true,
  );
  const manifest = join(directory, "minutes.json");
  const bytes = await readFile(manifest);
  try {
    const changed = JSON.parse(bytes.toString("utf8"));
    changed.rows += 1;
    await writeFile(manifest, JSON.stringify(changed));
    const rejected = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "research.dataset.use", params: { id: original.id } },
    });
    expect((await rejected.json()).error).toBeTruthy();
    expect((await rpc(page.request, "runtime.snapshot")).datasets).toEqual(current.datasets);
  } finally {
    await writeFile(manifest, bytes);
  }
});

test("saving a portfolio with different trading days is rejected without publishing it", async ({
  page,
}) => {
  await page.goto("/");
  const one = await seedHistory(page.request, [100, 101], "saved-days-one");
  const two = await seedHistory(page.request, [100, 101], "saved-days-two", {
    product: "cu",
    day: "2026-09-28",
    keep: true,
  });
  await rpc(page.request, "research.dataset.select", one);
  await rpc(page.request, "research.dataset.select", two);
  const before = (await rpc(page.request, "research.dataset.saved")).saved_datasets;
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await page.getByRole("button", { name: "新建回测", exact: true }).click();
  await page.getByText("保存当前选择", { exact: true }).click();
  await page.getByLabel("数据集名称", { exact: true }).fill("不一致区间");
  await page.getByRole("button", { name: "保存数据集", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("各合约的交易日不一致，请调整所选区间。");
  expect((await rpc(page.request, "research.dataset.saved")).saved_datasets).toEqual(before);
});
