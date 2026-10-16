// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const { enterWorkbench, testNode } = require("./electron_startup.cjs");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
const execFile = require("node:util").promisify(require("node:child_process").execFile);
(async () => {
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-history-native-"));
  // Fixture tasks must pin the exact plugins bundled with this desktop, even
  // when ASTERION_CPP_BUILD selects a different build configuration.
  const nativeDirectory = path.resolve("build/electron-resources/native");
  const pluginDirectory = path.join(nativeDirectory, "plugins");
  let application;
  try {
    application = await electron.launch({
      recordVideo: process.env.ASTERION_UI_VIDEO_DIR
        ? {
            dir: path.join(process.env.ASTERION_UI_VIDEO_DIR, "electron_history_archive"),
            size: { width: 1440, height: 900 },
          }
        : undefined,
      executablePath: require("../../scripts/desktop/electron-path.cjs")(),
      args: [path.resolve("apps/clients/terminal/electron"), `--user-data-dir=${temp}/ui`],
      env: {
        ...process.env,
        ASTERION_NODE_DIRECTORY: `${temp}/node`,
        ...testNode,
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
    await enterWorkbench(page);
    await call("node.data_tasks.local.open");
    // The fixture seeds the daily table under the sources the installed data
    // source offers: those are the ones the Terminal's pages read.
    const offered = (await call("data.tables.coverage")).table_coverage.sources;
    const table = offered.find(item => item.table === "futures.daily");
    const source = name =>
      offered.find(item => item.table === name && item.plugin_id === table.plugin_id).source;
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
      [
        "--directory",
        service.directory,
        "--daily-source",
        table.source,
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
            t => t.id === "native-queued-minutes",
          )?.state,
        { timeout: 30000 },
      )
      .toBe("succeeded");
    const archive = await call("data.datasets", {
      venue: "SHFE",
      product: "",
      contract_id: "",
      source: "",
    });
    assert.equal(archive.history_datasets.length, 2);
    const minute = archive.history_datasets.find(x => x.contract_id === "SHFE/cu/2023-10");
    assert.equal(minute.contract_id, "SHFE/cu/2023-10");
    const reply = await call("data.minutes.page", {
      id: minute.id,
      offset: 0,
      limit: 10,
      start: "",
      end: "",
      include_macd: false,
    });
    assert.equal(reply.history_page.bars.length, 10);
    assert.equal(reply.history_page.id, minute.id);
    await page
      .getByRole("navigation", { name: "业务工作区" })
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
    const panel = page.getByRole("region", { name: "历史数据仓库", exact: true });
    await expect(panel.locator(".publication-row")).toHaveCount(2);
    await panel.getByLabel("合约身份", { exact: true }).fill("SHFE/cu/2023-10");
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
    const filter = {
      venue: "",
      product: "",
      contract_id: minute.contract_id,
      source: minute.source,
    };
    assert.equal((await call("data.datasets", filter)).history_datasets[0].id, minute.id);
    const stoppedTask = (await call("runtime.snapshot")).nodes
      .find(node => node.id === "local")
      .health.services.find(item => item.id === "task");
    assert.equal(stoppedTask.state, "stopped");
    await call("node.action", { id: "local", service: "historical-data", action: "stop" });
    await call("node.data_tasks.local.open");
    await expect.poll(async () => (await call("runtime.snapshot")).data?.online).toBe(true);
    assert.deepEqual((await call("data.datasets", filter)).history_datasets, [minute]);
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
