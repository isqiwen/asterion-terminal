// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
const execFile = require("node:util").promisify(require("node:child_process").execFile);
(async () => {
  assert.equal(process.platform, "darwin");
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-history-native-"));
  let application;
  try {
    application = await electron.launch({
      executablePath: require("electron"),
      args: [path.resolve("apps/clients/terminal/electron"), `--user-data-dir=${temp}/ui`],
      env: {
        ...process.env,
        ASTERION_NODE_DIRECTORY: `${temp}/node`,
        ASTERION_TEST_NODE_ISOLATED: "1",
      },
      timeout: 30000,
    });
    const page = await application.firstWindow();
    const errors = [];
    page.on("pageerror", error => errors.push(String(error)));
    await page.waitForFunction(() => !!window.asterionDesktop);
    const call = async (method, params = {}, rejected = false) => {
      const response = await page.evaluate(
        async ({ method, params }) =>
          JSON.parse(
            await window.asterionDesktop.request(JSON.stringify({ version: 1, method, params })),
          ),
        { method, params },
      );
      if (rejected) {
        assert.ok(response.error, `expected ${method} to reject`);
        return response.error;
      }
      assert.equal(response.error, undefined, JSON.stringify(response.error));
      return response.result;
    };
    await page.getByRole("button", { name: "开始设置", exact: true }).click();
    await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 60000 });
    await call("research.local");
    const stopped = await call("node.action", { id: "local", service: "research", action: "stop" });
    const service = stopped.nodes
      .find(n => n.id === "local")
      .health.services.find(s => s.id === "research");
    const root = await fs.realpath(`${temp}/node`);
    assert.equal(
      path.relative(root, await fs.realpath(service.directory)),
      path.join("services", "research", "ledger"),
    );
    await execFile(
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", "asterion_test_minutes"),
      ["--directory", service.directory],
      {
        env: { ...process.env, ASTERION_NODE_DIRECTORY: root, ASTERION_TEST_NODE_ISOLATED: "1" },
        timeout: 15000,
      },
    );
    await call("research.local");
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).research?.tasks.find(
            t => t.id === "native-daily-fixture",
          )?.state,
        { timeout: 30000 },
      )
      .toBe("succeeded");
    const archive = await call("research.datasets", {
      venue: "SHFE",
      product: "cu",
      contract_id: "",
      source: "",
    });
    assert.equal(archive.history_datasets.length, 2);
    const daily = archive.history_datasets.find(x => x.interval_minutes === 0);
    assert.equal(daily.contract_id, "SHFE/cu/2023-10");
    const reply = await call("research.daily.page", {
      id: daily.id,
      archive: true,
      offset: 0,
      limit: 10,
      start: "",
      end: "",
      include_macd: false,
      period: "day",
    });
    assert.equal(reply.daily_page.bars.length, 10);
    assert.equal(reply.daily_page.id, daily.id);
    await page
      .getByRole("navigation", { name: "业务工作区" })
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
    const panel = page.getByRole("region", { name: "历史数据仓库", exact: true });
    await expect(panel.locator(".publication-row")).toHaveCount(2);
    await panel.getByLabel("数据源", { exact: true }).fill("tushare.fut_daily");
    await panel.getByRole("button", { name: "查询", exact: true }).click();
    await expect(panel.locator(".publication-row")).toHaveCount(1);
    await panel.getByRole("button", { name: "查看数据", exact: true }).click();
    const viewer = page.getByRole("region", { name: "历史数据查看", exact: true });
    await expect(viewer.locator("tbody tr")).toHaveCount(100);
    await fs.mkdir("build/history-archive", { recursive: true });
    await page.screenshot({ path: "build/history-archive/native-archive.png" });
    assert.deepEqual(errors, []);
    // Publication identity is stable through service restart.
    await call("node.action", { id: "local", service: "research", action: "stop" });
    await call("research.local");
    const reopened = await call("research.datasets", {
      venue: "",
      product: "",
      contract_id: daily.contract_id,
      source: daily.source,
    });
    assert.equal(reopened.history_datasets[0].id, daily.id);
    // Native risk deployment uses explicit test history, never a production import.
    await call("node.action", { id: "local", service: "research", action: "stop" });
    const seeded = await execFile(
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", "asterion_test_history"),
      [
        "--directory",
        service.directory,
        "--id",
        "native-risk",
        "--price",
        "100",
        "101",
        "102",
        "101",
        "100",
        "101",
        "103",
      ],
      {
        env: { ...process.env, ASTERION_NODE_DIRECTORY: root, ASTERION_TEST_NODE_ISOLATED: "1" },
        timeout: 15000,
      },
    );
    await call("research.local");
    await call("research.dataset.select", JSON.parse(seeded.stdout));
    const policy = {
      deposit: "10000",
      contracts: [
        {
          venue: "SHFE",
          symbol: "rb2610",
          margin_per_lot: "100",
          open_fee: "2",
          close_today_fee: "3",
          close_yesterday_fee: "4",
          margin_rate: "0",
          open_fee_rate: "0",
          close_today_fee_rate: "0",
          close_yesterday_fee_rate: "0",
        },
      ],
      max_order_quantity: "1",
      max_gross_quantity: "10",
      max_working_orders: "10",
    };
    await call("research.submit", {
      id: "native-risk-backtest",
      fast: 1,
      slow: 3,
      quantity: "1",
      ...policy,
    });
    const task = async () =>
      (await call("runtime.snapshot")).research.tasks.find(t => t.id === "native-risk-backtest");
    await expect
      .poll(
        async () => {
          const current = await task();
          assert.ok(
            ["queued", "running", "succeeded"].includes(current.state),
            JSON.stringify(current),
          );
          return current.state;
        },
        { timeout: 30000 },
      )
      .toBe("succeeded");
    const completed = await task();
    assert.match(completed.risk_artifact, /^[a-f0-9]{64}$/);
    const crypto = require("node:crypto");
    const digest = async file =>
      crypto
        .createHash("sha256")
        .update(await fs.readFile(file))
        .digest("hex");
    assert.equal(
      await digest(path.join(service.directory, "native-risk-backtest", "risk-plugin.dylib")),
      completed.risk_artifact,
    );
    const account = path.join(temp, "paper");
    await fs.mkdir(account);
    await call("paper.create", { directory: account, ...policy });
    await call("paper.act", { request_id: "advance", action: "advance" });
    const before = (await call("runtime.snapshot")).paper;
    const denied = await call(
      "paper.act",
      {
        request_id: "too-large",
        action: "submit",
        order_id: "oversized",
        venue: "SHFE",
        symbol: "rb2610",
        side: "buy",
        offset: "open",
        quantity: "2",
        price: "100",
      },
      true,
    );
    assert.match(denied.message, /risk rejected/);
    assert.deepEqual((await call("runtime.snapshot")).paper, before);
    assert.equal(
      await digest(path.join(account, "plugins", "risk-plugin.dylib")),
      completed.risk_artifact,
    );
    await call("paper.close");
    await call("paper.open", { directory: account });
    assert.deepEqual((await call("runtime.snapshot")).paper, before);
    await call("paper.close");
    await call("node.action", { id: "local", service: "research", action: "stop" });
    await call("research.local");
    assert.equal((await task()).risk_artifact, completed.risk_artifact);
    assert.equal((await task()).state, "succeeded");
    assert.deepEqual(errors, []);
    console.log(
      "Native risk: Agent deployment, backtest, pinned artifacts, denied oversized order and recovery passed",
    );
    console.log(
      "Native history archive: source-isolated immutable revisions, filtered UI, Protobuf pages and restart passed",
    );
  } finally {
    if (application) await application.close();
    try {
      process.kill(
        Number(await fs.readFile(path.join(temp, "node", "agent.pid"), "utf8")),
        "SIGTERM",
      );
      await new Promise(resolve => setTimeout(resolve, 2000));
    } catch (error) {
      if (!["ENOENT", "ESRCH"].includes(error.code)) throw error;
    }
    await fs.rm(temp, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
