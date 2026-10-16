// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const { testNode } = require("./electron_startup.cjs");
const path = require("node:path");
const fs = require("node:fs/promises");
const { promisify } = require("node:util");
const execFile = promisify(require("node:child_process").execFile);

module.exports = async function checkNativeHistory(page, temp, capture, pluginDirectory) {
  const request = (method, params = {}) =>
    page.evaluate(
      async ({ method, params }) =>
        JSON.parse(
          await window.asterionDesktop.request(JSON.stringify({ version: 1, method, params })),
        ),
      { method, params },
    );
  const call = async (method, params = {}) => {
    const response = await request(method, params);
    assert.equal(response.error, undefined, JSON.stringify(response.error));
    return response.result;
  };
  await call("node.data_tasks.local.open");
  // The daily table is seeded under the sources the installed data source
  // offers: those are the ones the Terminal's pages read.
  const offered = (await call("data.tables.coverage")).table_coverage.sources;
  const daily = offered.find(item => item.table === "futures.daily");
  const source = table =>
    offered.find(item => item.table === table && item.plugin_id === daily.plugin_id).source;
  const stopped = await call("node.action", { id: "local", service: "task", action: "stop" });
  await call("node.action", { id: "local", service: "historical-data", action: "stop" });
  const service = stopped.nodes
    .filter(node => node.id === "local")
    .flatMap(node => node.health?.services ?? [])
    .find(service => service.id === "task");
  assert.ok(service, "isolated Agent did not report its task service");
  const root = await fs.realpath(path.join(temp, "node"));
  assert.equal(
    path.relative(root, await fs.realpath(service.directory)),
    path.join("services", "task", "ledger"),
  );
  assert.equal(service.state, "stopped");
  const seeded = await execFile(
    process.env.ASTERION_TEST_MINUTE_FIXTURE ||
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", "asterion_test_minutes"),
    [
      "--directory",
      service.directory,
      "--daily-source",
      daily.source,
      "--calendar-source",
      source("futures.calendar"),
      "--contracts-source",
      source("futures.contracts"),
    ],
    {
      env: {
        ...process.env,
        ASTERION_NODE_DIRECTORY: root,
        ...testNode,
        // Authorization pins the actual desktop plugin bytes, including staging/signing.
        ASTERION_PLUGIN_DIRECTORY: pluginDirectory,
      },
      timeout: 10000,
    },
  );
  assert.match(seeded.stdout, /queued minute fixture/);
  assert.ok((await fs.stat(path.join(service.directory, "tasks.sqlite"))).isFile());
  await call("node.data_tasks.local.open");
  await expect
    .poll(
      async () => {
        const snapshot = await call("runtime.snapshot");
        const task = snapshot.task_service?.tasks.find(task => task.id === "native-queued-minutes");
        return task && { state: task.state, error: task.error || snapshot.task_service.error };
      },
      {
        timeout: 20000,
        message: "Agent must dispatch the queued minute worker and publish its result",
      },
    )
    .toEqual({ state: "succeeded", error: "" });
  const queuedTask = (await call("runtime.snapshot")).task_service.tasks.find(
    task => task.id === "native-queued-minutes",
  );
  assert.equal(queuedTask.attempt, 1);
  assert.equal(queuedTask.completed, queuedTask.total);
  assert.match(queuedTask.result_digest, /^[0-9a-f]{64}$/);
  // Read through the owning service: SQLite uses an exclusive writer lock.
  await call("node.action", { id: "local", service: "task", action: "stop" });
  await call("node.data_tasks.local.open");
  await expect
    .poll(async () =>
      (await call("runtime.snapshot")).task_service.tasks.find(t => t.id === queuedTask.id),
    )
    .toMatchObject({
      state: "succeeded",
      attempt: 1,
      result_digest: queuedTask.result_digest,
    });
  const minuteDatasetId = (await call("runtime.snapshot")).task_service.tasks.find(
    task => task.id === "native-minute-fixture",
  ).history_dataset_id;
  const pageQuery = offset => ({
    id: minuteDatasetId,
    offset,
    limit: 100,
    start: "",
    end: "",
    include_macd: false,
  });
  const [firstRead, lastRead, snapshot] = await Promise.all([
    request("data.minutes.page", pageQuery(0)),
    request("data.minutes.page", pageQuery(100)),
    call("runtime.snapshot"),
  ]);
  assert.equal(firstRead.error, undefined, JSON.stringify(firstRead.error));
  assert.equal(lastRead.error, undefined, JSON.stringify(lastRead.error));
  const first = firstRead.result;
  const last = lastRead.result;
  assert.equal(first.history_page.bars.length, 100);
  assert.equal(last.history_page.bars.length, 20);
  assert.equal(first.history_page.bars[0].open, "100.00000001");
  assert.equal(first.history_page.bars[0].amount, "12345678.12345678");
  assert.equal(snapshot.history_page, null);
  const empty = await call("data.minutes.page", {
    id: minuteDatasetId,
    offset: 0,
    limit: 100,
    start: "2023-08-25 09:30:01",
    end: "2023-08-25 09:30:59",
    include_macd: true,
  });
  assert.equal(empty.history_page.matched_rows, 0);
  assert.deepEqual(empty.history_page.bars, []);
  assert.equal(empty.history_page.total_rows, 120);

  const nav = page.getByRole("navigation", { name: "业务工作区" });
  await nav.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据", exact: true }).click();
  const openMinutes = page
    .locator('[data-task-id="native-minute-fixture"]')
    .getByRole("button", { name: "查看数据", exact: true });
  await openMinutes.click({ timeout: 15000 });
  const viewer = page.getByRole("region", { name: "历史数据查看", exact: true });
  await expect(viewer.getByRole("img", { name: "合约 K 线与成交量" })).toBeVisible();
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  await expect(viewer.locator("tbody tr").first()).toContainText("100.00000001");
  await viewer.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(20);
  await expect(viewer.locator(".history-pagination")).toContainText("101–120 / 120");
  await call("node.action", { id: "local", service: "historical-data", action: "stop" });
  await expect(viewer.getByRole("status")).toContainText("历史数据服务未连接", { timeout: 15000 });
  await expect(viewer.locator("tbody tr")).toHaveCount(20);
  await expect(viewer.getByRole("button", { name: "上一页", exact: true })).toBeDisabled();
  await call("node.data_tasks.local.open");
  await expect(viewer).toHaveCount(0, { timeout: 15000 });
  await openMinutes.click();
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  await viewer.getByLabel("开始时间（北京时间）").fill("2023-08-25T13:00");
  await viewer.getByLabel("结束时间（北京时间）").fill("2023-08-25T13:09");
  await viewer.getByRole("button", { name: "查看区间", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(10);
  await expect(viewer.locator("tbody tr").first()).toContainText("13:00:00");
  await viewer.getByRole("button", { name: "全部时间", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  await expect(viewer.getByLabel("开始时间（北京时间）")).toHaveValue("");
  await capture(page, "native-history-viewer");

  await nav.getByRole("button", { name: "市场", exact: true }).click();
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  const board = page.locator(".history-market-board");
  await expect(board.getByRole("img", { name: "合约历史 K 线", exact: true })).toBeVisible();
  await expect(board.locator(".market-chart-title strong")).toHaveText("SHFE/cu/2023-10");
  await board.getByRole("button", { name: "1 min", exact: true }).click();
  await expect(board.locator("[data-oscillator]")).toHaveCount(1);
  await expect(board.locator(".market-history-pages")).toContainText("1–100 / 120");
  await board.getByRole("button", { name: "向后", exact: true }).click();
  await expect(board.locator(".market-history-pages")).toContainText("101–120 / 120");
  await expect(board.getByRole("img", { name: "合约历史 K 线", exact: true })).toBeVisible();
  const indicators = await call("data.minutes.page", {
    id: minuteDatasetId,
    offset: 100,
    limit: 20,
    start: "",
    end: "",
    include_macd: true,
  });
  const macd = indicators.history_page.bars.at(-1).macd;
  assert.ok(macd && Math.abs(macd.diff) > 0.01, "fixture must exercise nonzero MACD values");
  await board.getByRole("img", { name: "合约历史 K 线", exact: true }).press("End");
  const legend = board.locator("[data-oscillator-legend]");
  await expect(legend).toContainText(`DIFF: ${macd.diff.toFixed(2)}`);
  await expect(legend).toContainText(`DEA: ${macd.signal.toFixed(2)}`);
  await expect(legend).toContainText(`MACD: ${macd.histogram.toFixed(2)}`);
  await capture(page, "native-history-market");

  await board.getByRole("button", { name: "日K", exact: true }).click();
  await expect(board.getByRole("button", { name: "日K", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(board.locator(".market-history-pages")).toContainText("1–100 / 120");
  await board.getByRole("button", { name: "向后", exact: true }).click();
  await expect(board.locator(".market-history-pages")).toContainText("101–120 / 120");
  // The days are the market-wide daily table's, under the snapshot the
  // coverage names now.
  const coverage = (await call("data.tables.coverage")).table_coverage;
  const table = {
    snapshot: coverage.snapshot,
    daily_source: daily.source,
    calendar_source: source("futures.calendar"),
    contracts_source: source("futures.contracts"),
  };
  const days = (
    await call("data.tables.daily", {
      daily: table,
      contract: "SHFE/cu/2023-10",
      period: "day",
      offset: 100,
      limit: 20,
    })
  ).table_daily;
  assert.equal(days.candles[0].trading_day, "2023-04-11");
  assert.equal(days.candles[0].open, "100");
  assert.equal((await call("runtime.snapshot")).table_daily, undefined);
  await board.getByRole("img", { name: "合约历史 K 线", exact: true }).press("End");
  await expect(legend).toContainText(`DIFF: ${days.candles.at(-1).macd.diff.toFixed(2)}`);
  await expect(legend).toContainText(`MACD: ${days.candles.at(-1).macd.histogram.toFixed(2)}`);
  await expect(board.locator(".market-chart-source")).toContainText("全市场日线表");
  await capture(page, "native-daily-market");
  for (const [label, period, count] of [
    ["周线", "week", 18],
    ["月线", "month", 4],
    ["季线", "quarter", 2],
    ["年线", "year", 1],
  ]) {
    await board.getByRole("button", { name: label, exact: true }).click();
    await expect(board.getByRole("button", { name: label, exact: true })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    await expect(board.locator(".market-history-pages")).toContainText(`1–${count} / ${count}`);
    const grouped = (
      await call("data.tables.daily", {
        daily: table,
        contract: "SHFE/cu/2023-10",
        period,
        offset: 0,
        limit: 100,
      })
    ).table_daily;
    await expect(board.locator("[data-oscillator-empty]")).toHaveText("当前区间无 MACD 数据");
    await expect(board.locator("[data-oscillator-axis]")).toHaveCount(0);
    assert.equal(grouped.period, period);
    assert.equal(grouped.total, count);
    assert.equal(
      grouped.candles[0].trading_day,
      period === "week"
        ? "2023-01-01"
        : period === "month"
          ? "2023-01-31"
          : period === "quarter"
            ? "2023-03-31"
            : "2023-04-30",
    );
    if (period === "month") {
      // January: 31 days of 20 lots.
      assert.equal(grouped.candles[0].volume, "620");
      await capture(page, "native-monthly-market");
    }
    if (period === "year") assert.equal(grouped.candles[0].volume, "2400");
    await expect(board.getByRole("img", { name: "合约历史 K 线", exact: true })).toBeVisible();
  }
  // The current chart uses a canvas; single-candle navigation must keep the
  // one published observation and its date visible in the accessible readout.
  const yearly = board.getByRole("img", { name: "合约历史 K 线", exact: true });
  await expect(yearly.locator("canvas").first()).toBeVisible();
  await expect(yearly).toHaveAttribute("data-visible-count", "1");
  await yearly.press("Home");
  await expect(board.locator(".price-chart-cursor")).toContainText("2023-04-30");
  await yearly.press("End");
  await expect(board.locator(".price-chart-cursor")).toContainText("2023-04-30");
  await capture(page, "native-yearly-market");
  console.log(
    "Electron minute/daily history: Agent-dispatched minute worker, real Task Service persistence, native parallel queries, exact decimals, paging, service restart and MACD charts passed",
  );
};
