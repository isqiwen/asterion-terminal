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
  // Fixture tasks must pin the exact plugins bundled with this desktop, even
  // when ASTERION_CPP_BUILD selects a different build configuration.
  const nativeDirectory = path.resolve("build/electron-resources/native");
  const pluginDirectory = path.join(nativeDirectory, "plugins");
  let application;
  try {
    application = await electron.launch({
      executablePath: require("../scripts/electron-path.cjs")(),
      args: [path.resolve("apps/clients/terminal/electron"), `--user-data-dir=${temp}/ui`],
      env: {
        ...process.env,
        ASTERION_NODE_DIRECTORY: `${temp}/node`,
        ASTERION_TEST_NODE_ISOLATED: "1",
        ASTERION_PLUGIN_DIRECTORY: pluginDirectory,
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
    await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 60000 });
    await call("node.data_tasks.local.open");
    const stopped = await call("node.action", { id: "local", service: "task", action: "stop" });
    await call("node.action", { id: "local", service: "historical-data", action: "stop" });
    const service = stopped.nodes
      .find(n => n.id === "local")
      .health.services.find(s => s.id === "task");
    const root = await fs.realpath(`${temp}/node`);
    assert.equal(
      path.relative(root, await fs.realpath(service.directory)),
      path.join("services", "task", "ledger"),
    );
    await execFile(
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", "asterion_test_minutes"),
      ["--directory", service.directory],
      {
        env: {
          ...process.env,
          ASTERION_NODE_DIRECTORY: root,
          ASTERION_TEST_NODE_ISOLATED: "1",
          ASTERION_PLUGIN_DIRECTORY: pluginDirectory,
        },
        timeout: 15000,
      },
    );
    await call("node.data_tasks.local.open");
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).task_service?.tasks.find(
            t => t.id === "native-daily-fixture",
          )?.state,
        { timeout: 30000 },
      )
      .toBe("succeeded");
    const archive = await call("data.datasets", {
      venue: "SHFE",
      product: "cu",
      contract_id: "",
      source: "",
    });
    assert.equal(archive.history_datasets.length, 2);
    const daily = archive.history_datasets.find(x => x.interval_minutes === 0);
    assert.equal(daily.contract_id, "SHFE/cu/2023-10");
    const reply = await call("data.daily.page", {
      id: daily.id,
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
    // Historical versions belong to Data and remain readable while Task is stopped.
    await call("node.action", { id: "local", service: "task", action: "stop" });
    const filter = { venue: "", product: "", contract_id: daily.contract_id, source: daily.source };
    assert.equal((await call("data.datasets", filter)).history_datasets[0].id, daily.id);
    const stoppedTask = (await call("runtime.snapshot")).nodes
      .find(node => node.id === "local")
      .health.services.find(item => item.id === "task");
    assert.equal(stoppedTask.state, "stopped");
    await call("node.action", { id: "local", service: "historical-data", action: "stop" });
    await call("node.data_tasks.local.open");
    await expect.poll(async () => (await call("runtime.snapshot")).data?.online).toBe(true);
    assert.deepEqual((await call("data.datasets", filter)).history_datasets, [daily]);
    assert.deepEqual(errors, []);
    console.log(
      "Native history archive: source-isolated immutable revisions, filtered UI, Protobuf pages, independent ownership and restart passed",
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
