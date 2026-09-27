import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("strategy runs through Agent and can relinquish its paper account", async ({ page }) => {
  test.setTimeout(120000);
  const folder = await mkdtemp(join(tmpdir(), "asterion-strategy-ui-"));
  const csv = join(folder, "ticks.csv"),
    directory = join(folder, "account");
  await mkdir(directory);
  await writeFile(
    csv,
    "timestamp_ns,price,quantity\n" +
      Array.from(
        { length: 300 },
        (_, i) => `${i + 1},${[100, 101, 100, 102, 99, 103][i % 6]},1\n`,
      ).join(""),
  );
  const rpc = async (method: string, params: Record<string, unknown> = {}) => {
    const response = await page.request.post("/__asterion/api", {
      data: { version: 1, method, params },
    });
    const body = await response.json();
    expect(body.error).toBeUndefined();
    return body.result;
  };
  let service = "";
  try {
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible();
    await rpc("futures.inspect_csv", {
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
    await rpc("paper.create", {
      directory,
      deposit: "10000",
      margin_per_lot: "100",
      open_fee: "2",
      close_today_fee: "3",
      close_yesterday_fee: "4",
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
    await expect(page.getByRole("button", { name: "回放下一笔", exact: true })).toBeDisabled();
    await expect(page.getByRole("button", { name: "提交模拟委托", exact: true })).toBeDisabled();
    await page.screenshot({ path: join(__dirname, "../test-results/strategy-running.png") });
    await panel.getByRole("button", { name: "撤销策略授权", exact: true }).click();
    await expect(panel.getByRole("button", { name: "撤销策略授权", exact: true })).toHaveCount(0);
    await expect(page.getByRole("button", { name: "回放下一笔", exact: true })).toBeEnabled();
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
    await rm(folder, { recursive: true, force: true });
  }
});

test("published calendar drives strategy settlement and survives service restart", async ({
  page,
}) => {
  test.setTimeout(120000);
  const folder = await mkdtemp(join(tmpdir(), "asterion-scheduled-ui-"));
  const directory = join(folder, "account"),
    csv = join(folder, "ticks.csv"),
    calendar = join(folder, "calendar.csv");
  await mkdir(directory);
  const start = 1790298000000000000n;
  await writeFile(
    csv,
    "timestamp_ns,price,quantity\n" +
      [100, 101, 100, 102, 99, 103]
        .map((price, i) => `${start + BigInt(i < 3 ? i : 259200 + i - 3) * 1000000000n},${price},1`)
        .join("\n") +
      "\n",
  );
  await writeFile(
    calendar,
    "trading_day,session_begin,session_end,settlement_price,schedule_source,settlement_source\n2026-09-25,2026-09-25T09:00:00+08:00,2026-09-25T09:00:03+08:00,105,fixture,fixture\n2026-09-28,2026-09-28T09:00:00+08:00,2026-09-28T09:00:03+08:00,110,fixture,fixture\n",
  );
  const rpc = async (method: string, params: Record<string, unknown> = {}) => {
    const body = await (
      await page.request.post("/__asterion/api", { data: { version: 1, method, params } })
    ).json();
    expect(body.error).toBeUndefined();
    return body.result;
  };
  let service = "";
  try {
    await page.goto("/");
    await rpc("futures.inspect_csv", {
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
    await rpc("research.local");
    const id = `calendar-${crypto.randomUUID()}`;
    await rpc("research.calendar.submit", { id, path: calendar });
    await rm(calendar);
    await expect
      .poll(
        async () =>
          (await rpc("runtime.snapshot")).research.tasks.find(
            (task: { id: string }) => task.id === id,
          )?.state,
        { timeout: 20000 },
      )
      .toBe("succeeded");
    await rpc("paper.create", {
      directory,
      deposit: "1000",
      margin_per_lot: "100",
      open_fee: "2",
      close_today_fee: "3",
      close_yesterday_fee: "4",
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
    await panel.getByLabel("日程来源", { exact: true }).selectOption(id);
    await panel.getByRole("button", { name: "授权并运行", exact: true }).click();
    await expect
      .poll(async () => (await rpc("runtime.snapshot")).strategy?.phase, { timeout: 20000 })
      .toBe("completed");
    const state = await rpc("runtime.snapshot");
    service = state.strategy.id;
    expect(state.paper.balance).toBe("1024");
    expect(state.paper.fees).toBe("6");
    expect(state.paper.replay.settled_days).toBe(2);
    expect(state.paper.strategy.active).toBe(false);
    await expect(panel.getByText("结算日程 · 2 / 2", { exact: true })).toBeVisible();
    await panel.getByText("结算日程 · 2 / 2", { exact: true }).click();
    await expect(panel.getByText("calendar.csv", { exact: true })).toBeVisible();
    await panel.screenshot({ path: join(__dirname, "../test-results/strategy-calendar.png") });
    await rpc("node.action", { id: "local", service, action: "restart" });
    await expect
      .poll(async () => (await rpc("runtime.snapshot")).strategy?.phase, { timeout: 20000 })
      .toBe("completed");
    expect((await rpc("runtime.snapshot")).paper).toEqual(state.paper);
    await rpc("paper.close");
    await rpc("paper.open", { directory });
    expect((await rpc("runtime.snapshot")).paper).toEqual(state.paper);
    await rpc("paper.close");
    const takeover = join(folder, "takeover");
    await mkdir(takeover);
    await rpc("paper.create", {
      directory: takeover,
      deposit: "1000",
      margin_per_lot: "100",
      open_fee: "2",
      close_today_fee: "3",
      close_yesterday_fee: "4",
      max_order_quantity: "100",
      max_gross_quantity: "100",
      max_working_orders: "100",
    });
    await rpc("paper.act", {
      request_id: "calendar",
      action: "replay_calendar",
      publication: state.paper.replay.publication,
    });
    await rpc("paper.act", {
      request_id: "grant",
      action: "strategy_grant",
      grant_id: "takeover",
      strategy_id: "takeover",
      stream_id: "takeover",
      dataset_revision: state.paper.strategy.dataset_revision,
      max_quantity: "1",
    });
    for (let i = 0; i < 3; i++)
      await rpc("paper.act", { request_id: `advance${i}`, action: "advance" });
    await rpc("paper.act", {
      request_id: "revoke",
      action: "strategy_revoke",
      grant_id: "takeover",
    });
    await page.getByRole("button", { name: "查看服务连接", exact: true }).click();
    await page.getByRole("button", { name: "重新检测", exact: true }).click();
    await page.keyboard.press("Escape");
    await expect(page.getByRole("button", { name: "回放下一笔", exact: true })).toBeDisabled();
    await page.getByText("模拟规则与手动结算", { exact: true }).click();
    await expect(page.getByLabel("结算价", { exact: true })).toHaveCount(0);
    await page.getByRole("button", { name: "按日程结算", exact: true }).click();
    await expect(page.getByRole("button", { name: "回放下一笔", exact: true })).toBeEnabled();
    expect((await rpc("runtime.snapshot")).paper.replay.settled_days).toBe(1);
  } finally {
    if (service) await rpc("node.action", { id: "local", service, action: "stop" });
    await rpc("paper.close");
    await rm(folder, { recursive: true, force: true });
  }
});
