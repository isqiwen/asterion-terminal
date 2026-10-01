import { removeFolder } from "./cleanup";
import { rpc as call, seedDataset } from "./dataset-fixture";
import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("strategy runs through Agent and can relinquish its paper account", async ({ page }) => {
  test.setTimeout(120000);
  const folder = await mkdtemp(join(tmpdir(), "asterion-strategy-ui-"));
  const directory = join(folder, "account");
  await mkdir(directory);
  const rpc = (method: string, params: Record<string, unknown> = {}) =>
    call(page.request, method, params);
  let service = "";
  try {
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
      timeout: 60000,
    });
    await seedDataset(
      page.request,
      Array.from({ length: 300 }, (_, i) => [100, 101, 100, 102, 99, 103][i % 6]),
      "strategy",
    );
    await rpc("paper.create", {
      directory,
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
    await page.getByRole("button", { name: "查看服务连接", exact: true }).click();
    await page.getByRole("button", { name: "重新检测", exact: true }).click();
    await page.keyboard.press("Escape");
    await page.getByRole("button", { name: "交易", exact: true }).click();
    const panel = page.getByRole("region", { name: "策略运行", exact: true });
    await panel.getByLabel("短周期", { exact: true }).fill("1");
    await panel.getByLabel("长周期", { exact: true }).fill("2");
    await panel.getByRole("button", { name: "授权并运行", exact: true }).click();
    await expect(panel.getByRole("button", { name: "撤销策略授权", exact: true })).toBeEnabled({
      timeout: 20000,
    });
    service = (await rpc("runtime.snapshot")).strategy.id;
    await expect
      .poll(async () => (await rpc("runtime.snapshot")).strategy.processed, { timeout: 15000 })
      .toBeGreaterThan(0);
    await expect(page.getByRole("button", { name: "回放下一根", exact: true })).toBeDisabled();
    await expect(page.getByRole("button", { name: "提交模拟委托", exact: true })).toBeDisabled();
    await page.screenshot({ path: join(__dirname, "../test-results/strategy-running.png") });
    await panel.getByRole("button", { name: "撤销策略授权", exact: true }).click();
    await expect(panel.getByRole("button", { name: "撤销策略授权", exact: true })).toHaveCount(0);
    await expect(page.getByRole("button", { name: "回放下一根", exact: true })).toBeEnabled();
    const state = await rpc("runtime.snapshot");
    expect(state.paper.strategy.active).toBe(false);
    await expect(panel.getByRole("status")).toContainText("已暂停", { timeout: 30000 });
    await page.screenshot({ path: join(__dirname, "../test-results/strategy-revoked.png") });
    page = await openSettingsWindow(page);
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    const row = page
      .getByRole("table", { name: "节点服务状态" })
      .getByRole("row")
      .filter({ hasText: service });
    await row.getByText("程序更新", { exact: true }).click();
    await expect(row.getByRole("button", { name: "更新已停止的服务", exact: true })).toBeDisabled();
    await row.getByRole("button", { name: "停止", exact: true }).click();
    await expect(row.getByRole("button", { name: "更新已停止的服务", exact: true })).toBeEnabled();
    const ledger = (await rpc("runtime.snapshot")).paper;
    const completed = page.waitForResponse(
      response =>
        response.url().includes("/__asterion/api") &&
        response.request().postDataJSON()?.method === "node.update",
    );
    await row.getByRole("button", { name: "更新已停止的服务", exact: true }).click();
    expect((await (await completed).json()).error).toBeUndefined();
    expect((await rpc("runtime.snapshot")).paper).toEqual(ledger);
    await expect(row.getByRole("button", { name: "启动", exact: true })).toBeEnabled();
    await row.getByRole("button", { name: "启动", exact: true }).click();
    await expect(row.getByRole("button", { name: "停止", exact: true })).toBeEnabled();
  } finally {
    if (service) await rpc("node.action", { id: "local", service, action: "stop" });
    await rpc("paper.close");
    await removeFolder(folder);
  }
});
