import { test, expect } from "@playwright/test";
import { seedHistory, rpc } from "./dataset-fixture";

test("combine downloads, reject conflicts, save and run the restored input", async ({ page }) => {
  const first = await seedHistory(page.request, [100, 101, 102], "composed-first", {
    day: "2026-09-23",
  });
  const second = await seedHistory(page.request, [103, 104, 105], "composed-second", {
    day: "2026-09-24",
  });
  const conflict = await seedHistory(page.request, [900, 101, 102], "composed-conflict", {
    day: "2026-09-23",
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await page.getByRole("button", { name: "新建回测", exact: true }).click();
  const picker = page.getByRole("form", { name: "历史数据集" });
  await picker.getByLabel("K 线来源", { exact: true }).selectOption(first.source_dataset_ids[0]);
  await picker
    .getByLabel("结算价来源", { exact: true })
    .selectOption(first.settlement_dataset_ids[0]);
  await picker.getByLabel("最小变动价位", { exact: true }).fill("1");
  await picker.getByLabel("合约乘数", { exact: true }).fill("10");
  await picker.getByText("拼接更多下载", { exact: false }).click();
  const bars = picker.getByRole("group", { name: "补充 K 线", exact: true });
  const settlements = picker.getByRole("group", { name: "补充结算价", exact: true });
  await bars
    .getByRole("checkbox", { name: new RegExp(second.source_dataset_ids[0].slice(0, 8)) })
    .check();
  await settlements
    .getByRole("checkbox", { name: new RegExp(second.settlement_dataset_ids[0].slice(0, 8)) })
    .check();
  await picker.getByRole("button", { name: "使用此数据集", exact: true }).click();
  await expect(picker.getByRole("list", { name: "已选合约" })).toContainText("6 根 · 2 个交易日");
  const selected = (await rpc(page.request, "runtime.snapshot")).datasets;
  expect(selected[0].source_dataset_ids).toEqual(
    [...first.source_dataset_ids, ...second.source_dataset_ids].sort(),
  );
  await bars
    .getByRole("checkbox", { name: new RegExp(conflict.source_dataset_ids[0].slice(0, 8)) })
    .check();
  await picker.getByRole("button", { name: "更新此合约", exact: true }).click();
  await expect(picker.getByRole("alert")).toContainText("所选下载在同一时刻的 K 线不一致");
  expect((await rpc(page.request, "runtime.snapshot")).datasets).toEqual(selected);
  await bars
    .getByRole("checkbox", { name: new RegExp(conflict.source_dataset_ids[0].slice(0, 8)) })
    .uncheck();
  await picker.getByRole("button", { name: "更新此合约", exact: true }).click();
  await expect(picker.getByRole("alert")).toHaveCount(0);
  await page.getByText("保存当前选择", { exact: true }).click();
  await page.getByLabel("数据集名称", { exact: true }).fill("跨下载研究");
  await page.getByRole("button", { name: "保存数据集", exact: true }).click();
  await expect(
    page.getByText("数据集已保存，可在当前研究服务中重复使用。", { exact: true }),
  ).toBeVisible();
  const saved = (await rpc(page.request, "research.dataset.saved")).saved_datasets.find(
    (item: { name: string }) => item.name === "跨下载研究",
  );
  await picker.locator(".dataset-composition").scrollIntoViewIfNeeded();
  const rowsFit = await picker.locator(".dataset-segment").evaluateAll(rows =>
    rows.every(row => {
      const input = row.querySelector("input")!,
        text = row.querySelector("span")!;
      return (
        input.getBoundingClientRect().width <= 20 &&
        text.getBoundingClientRect().right <= row.getBoundingClientRect().right &&
        row.scrollWidth <= row.clientWidth + 1
      );
    }),
  );
  expect(rowsFit).toBe(true);
  await page.screenshot({
    path: "build/audit-review/composed-datasets/selection.png",
    fullPage: true,
  });
  await rpc(page.request, "research.dataset.clear");
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await rpc(page.request, "node.action", { id: "local", service: "research", action: "stop" });
  await rpc(page.request, "research.local");
  await rpc(page.request, "research.dataset.use", { id: saved.id });
  expect((await rpc(page.request, "runtime.snapshot")).datasets).toEqual(selected);
  await rpc(page.request, "research.submit", {
    id: "composed-backtest",
    fast: 1,
    slow: 3,
    quantity: "1",
    deposit: "10000",
    contracts: [
      {
        venue: "SHFE",
        symbol: "rb2610",
        cost_schedule: [
          {
            effective_from: "1970-01-01",
            source: "test fixture",
            values: {
              margin_per_lot: "100",
              open_fee: "2",
              close_today_fee: "3",
              close_yesterday_fee: "4",
              margin_rate: "0",
              open_fee_rate: "0",
              close_today_fee_rate: "0",
              close_yesterday_fee_rate: "0",
            },
          },
        ],
      },
    ],
    max_order_quantity: "100",
    max_gross_quantity: "100",
    max_working_orders: "100",
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research.tasks.find(
          (item: { id: string }) => item.id === "composed-backtest",
        )?.state,
    )
    .toBe("succeeded");
  const evidence = (await rpc(page.request, "research.result", { id: "composed-backtest" }))
    .research_result;
  expect(evidence.experiment.paper.contracts[0].dataset.source_dataset_ids).toEqual(
    selected[0].source_dataset_ids,
  );
});
