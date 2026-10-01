// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const path = require("node:path");
const fs = require("node:fs/promises");
const { promisify } = require("node:util");
const execFile = promisify(require("node:child_process").execFile);

module.exports = async function checkNativeHistory(page, temp, capture) {
  const call = async (method, params = {}) => {
    const response = await page.evaluate(
      async ({ method, params }) =>
        JSON.parse(
          await window.asterionDesktop.request(JSON.stringify({ version: 1, method, params })),
        ),
      { method, params },
    );
    assert.equal(response.error, undefined, JSON.stringify(response.error));
    return response.result;
  };
  await call("research.local");
  const stopped = await call("node.action", { id: "local", service: "research", action: "stop" });
  const service = stopped.nodes
    .filter(node => node.id === "local")
    .flatMap(node => node.health?.services ?? [])
    .find(service => service.id === "research");
  assert.ok(service, "isolated Agent did not report its research service");
  const root = await fs.realpath(path.join(temp, "node"));
  assert.equal(
    path.relative(root, await fs.realpath(service.directory)),
    path.join("services", "research", "ledger"),
  );
  assert.equal(service.state, "stopped");
  const seeded = await execFile(
    process.env.ASTERION_TEST_MINUTE_FIXTURE ||
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", "asterion_test_minutes"),
    ["--directory", service.directory],
    {
      env: { ...process.env, ASTERION_NODE_DIRECTORY: root, ASTERION_TEST_NODE_ISOLATED: "1" },
      timeout: 10000,
    },
  );
  assert.match(seeded.stdout, /queued daily fixture/);
  assert.ok((await fs.stat(path.join(service.directory, "tasks.sqlite"))).isFile());
  await call("research.local");
  await expect
    .poll(
      async () => {
        const snapshot = await call("runtime.snapshot");
        return snapshot.research?.tasks.find(task => task.id === "native-daily-fixture")?.state;
      },
      {
        timeout: 20000,
        message: "Agent must dispatch the queued daily worker and publish its result",
      },
    )
    .toBe("succeeded");
  const dailyTask = (await call("runtime.snapshot")).research.tasks.find(
    task => task.id === "native-daily-fixture",
  );
  assert.equal(dailyTask.attempt, 1);
  assert.equal(dailyTask.completed, dailyTask.total);
  assert.match(dailyTask.result_digest, /^[0-9a-f]{64}$/);
  // Read through the owning service: SQLite uses an exclusive writer lock.
  await call("node.action", { id: "local", service: "research", action: "stop" });
  await call("research.local");
  const recoveredDaily = (await call("runtime.snapshot")).research.tasks.find(
    t => t.id === dailyTask.id,
  );
  assert.equal(recoveredDaily.state, "succeeded");
  assert.equal(recoveredDaily.attempt, 1);
  assert.equal(recoveredDaily.result_digest, dailyTask.result_digest);
  const read = offset =>
    call("research.minutes.page", {
      id: "native-minute-fixture",
      offset,
      limit: 100,
      start: "",
      end: "",
      include_macd: false,
    });
  const [first, last, snapshot] = await Promise.all([read(0), read(100), call("runtime.snapshot")]);
  assert.equal(first.history_page.bars.length, 100);
  assert.equal(last.history_page.bars.length, 20);
  assert.equal(first.history_page.bars[0].open, "100.00000001");
  assert.equal(first.history_page.bars[0].amount, "12345678.12345678");
  assert.equal(snapshot.history_page, null);
  const empty = await call("research.minutes.page", {
    id: "native-minute-fixture",
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
  await page.getByRole("button", { name: "查看数据", exact: true }).click({ timeout: 15000 });
  const viewer = page.getByRole("region", { name: "历史数据查看", exact: true });
  await expect(viewer.getByRole("img", { name: "合约 K 线与成交量" })).toBeVisible();
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  await expect(viewer.locator("tbody tr").first()).toContainText("100.00000001");
  await viewer.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(20);
  await expect(viewer.locator(".history-pagination")).toContainText("101–120 / 120");
  await call("node.action", { id: "local", service: "research", action: "stop" });
  await expect(viewer.getByRole("status")).toContainText("历史数据服务未连接", { timeout: 15000 });
  await expect(viewer.locator("tbody tr")).toHaveCount(20);
  await expect(viewer.getByRole("button", { name: "上一页", exact: true })).toBeDisabled();
  await call("research.local");
  await expect(viewer).toHaveCount(0, { timeout: 15000 });
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
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
  const indicators = await call("research.minutes.page", {
    id: "native-minute-fixture",
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
  const daily = (
    await call("research.daily.page", {
      id: "native-daily-fixture",
      period: "day",
      offset: 100,
      limit: 20,
      start: "",
      end: "",
      include_macd: true,
    })
  ).daily_page;
  assert.equal(daily.bars[0].trading_day, "2023-04-11");
  assert.equal(daily.bars[0].amount, "12345678.12345678");
  assert.equal(daily.bars[0].settlement, null);
  assert.equal((await call("runtime.snapshot")).daily_page, null);
  await board.getByRole("img", { name: "合约历史 K 线", exact: true }).press("End");
  await expect(legend).toContainText(`DIFF: ${daily.bars.at(-1).macd.diff.toFixed(2)}`);
  await expect(legend).toContainText(`MACD: ${daily.bars.at(-1).macd.histogram.toFixed(2)}`);
  await expect(board.locator(".history-market-contracts")).toContainText("1 min / 日K");
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
      await call("research.daily.page", {
        id: "native-daily-fixture",
        period,
        offset: 0,
        limit: 100,
        start: "",
        end: "",
        include_macd: true,
      })
    ).daily_page;
    await expect(board.locator("[data-oscillator-empty]")).toHaveText("当前区间无 MACD 数据");
    await expect(board.locator("[data-oscillator-axis]")).toHaveCount(0);
    assert.equal(grouped.period, period);
    assert.equal(grouped.total_rows, count);
    assert.equal(
      grouped.bars[0].trading_day,
      period === "week"
        ? "2023-01-01"
        : period === "month"
          ? "2023-01-31"
          : period === "quarter"
            ? "2023-03-31"
            : "2023-04-30",
    );
    if (period === "month") {
      assert.equal(grouped.bars[0].amount, "382716021.82716018");
      await capture(page, "native-monthly-market");
    }
    if (period === "year") assert.equal(grouped.bars[0].amount, "1481481374.8148136");
    await expect(board.getByRole("img", { name: "合约历史 K 线", exact: true })).toBeVisible();
  }
  const dateAlignment = await board
    .getByRole("img", { name: "合约历史 K 线", exact: true })
    .evaluate(svg => {
      const text = svg.querySelector("[data-chart-time]").getBBox();
      const crosshair = svg.querySelector(".price-chart-crosshair").getPointAtLength(0);
      return Math.abs(text.x + text.width / 2 - crosshair.x);
    });
  assert.ok(dateAlignment < 1, "the single date label must align with its candle");
  await capture(page, "native-yearly-market");
  await nav.getByRole("button", { name: "数据", exact: true }).click();
  // Close the previously retained minute viewer before selecting another source.
  const back = page.getByRole("button", { name: "返回数据集", exact: true });
  if (await back.count()) await back.click();
  await page
    .getByRole("combobox", { name: "数据源", exact: true })
    .selectOption("tushare.fut_daily");
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
  await expect(viewer.getByRole("region", { name: "日线数据表", exact: true })).toBeVisible();
  await expect(viewer.locator("tbody tr").first()).toContainText("2023-01-01");
  await expect(viewer.locator("tbody tr").first().locator("td").nth(10)).toHaveText("—");
  await viewer.getByLabel("开始交易日期", { exact: true }).fill("2023-04-11");
  await viewer.getByLabel("结束交易日期", { exact: true }).fill("2023-04-20");
  await viewer.getByRole("button", { name: "查看区间", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(10);
  await expect(viewer.locator("tbody tr").first()).toContainText("2023-04-11");
  await capture(page, "native-daily-viewer");
  await page.getByRole("button", { name: "返回数据集", exact: true }).click();
  await page.getByRole("button", { name: "日线因子分析", exact: true }).click();
  await expect(page.getByRole("button", { name: "日线因子", exact: true })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect(page.getByLabel("日线来源", { exact: true })).toHaveValue("native-daily-fixture");
  await page.getByLabel("回看日线数", { exact: true }).fill("5");
  await page.getByLabel("未来日线数", { exact: true }).fill("1");
  await page.getByLabel("评价方式", { exact: true }).selectOption("holdout");
  await page.getByLabel("前段日线数", { exact: true }).fill("60");
  await page.getByRole("button", { name: "开始日线分析", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await call("runtime.snapshot")).research.tasks.find(task => task.kind === "daily_factor")
          ?.state,
      { timeout: 20000 },
    )
    .toBe("succeeded");
  const factorTask = (await call("runtime.snapshot")).research.tasks.find(
    task => task.kind === "daily_factor",
  );
  assert.equal(factorTask.attempt, 1);
  const taskTable = page.getByRole("region", { name: "研究任务", exact: true });
  await taskTable.getByRole("button", { name: "查看结果", exact: true }).click();
  const factorView = page.getByRole("region", { name: "日线因子结果", exact: true });
  await expect(factorView).toContainText("有效样本: 113");
  await expect(factorView).toContainText("跨界剔除: 1");
  const parts = factorView.getByRole("table", { name: "日线分区评价", exact: true });
  await expect(parts.locator("tbody tr").first()).toContainText("54");
  await expect(parts.locator("tbody tr").last()).toContainText("59");
  await factorView.getByText("逐日样本", { exact: true }).click();
  const rows = factorView.locator("details table tbody tr");
  await expect(rows).toHaveCount(50);
  await expect(rows.first()).toContainText("2023-01-06");
  await factorView.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(rows.first()).toContainText("2023-02-25");
  await capture(page, "native-daily-factor");
  await call("node.action", { id: "local", service: "research", action: "stop" });
  await call("research.local");
  await expect
    .poll(
      async () => {
        try {
          return (await call("research.result", { id: factorTask.id })).research_result?.result
            .samples.length;
        } catch {
          return 0;
        }
      },
      { timeout: 20000 },
    )
    .toBe(113);
  console.log(
    "Electron minute/daily history: Agent-dispatched daily worker, real Task Service persistence, native parallel queries, exact decimals, paging, service restart and MACD charts passed",
  );
};
