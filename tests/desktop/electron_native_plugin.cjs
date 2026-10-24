// Exercise the production Electron host with an isolated optional C plugin.
delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const { enterWorkbench, testNode } = require("./electron_startup.cjs");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
const build = path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug");
const library = process.platform === "darwin" ? ".dylib" : ".so";
(async () => {
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-plugin-native-"));
  const plugins = path.join(temp, "plugins");
  const installed = path.join(temp, "node/plugins");
  let application;
  try {
    await fs.mkdir(plugins);
    await fs.mkdir(installed, { recursive: true });
    const original = path.join(installed, "fixture" + library);
    await fs.copyFile(
      path.join(build, "tests/support/native-plugin/good/plugin_fixture_good" + library),
      original,
    );
    await fs.copyFile(
      path.join(build, "tests/support/native-plugin/bad_abi/plugin_fixture_bad_abi" + library),
      path.join(plugins, "broken" + library),
    );
    const launch = () =>
      electron.launch({
        recordVideo: process.env.ASTERION_UI_VIDEO_DIR
          ? {
              dir: path.join(process.env.ASTERION_UI_VIDEO_DIR, "electron_native_plugin"),
              size: { width: 1440, height: 900 },
            }
          : undefined,
        executablePath: require("../../scripts/desktop/electron-path.cjs")(),
        args: [path.resolve("apps/clients/terminal/electron"), `--user-data-dir=${temp}/ui`],
        env: {
          ...process.env,
          ASTERION_NODE_DIRECTORY: `${temp}/node`,
          ...testNode,
          ASTERION_PLUGIN_DIRECTORY: plugins,
        },
        timeout: 30000,
      });
    application = await launch();
    let page = await application.firstWindow();
    const errors = [];
    page.on("pageerror", error => errors.push(String(error)));
    const call = async (method, params = {}) => {
      const response = await page.evaluate(
        async ({ method, params }) =>
          JSON.parse(
            await window.asterionDesktop.request(JSON.stringify({ version: 1, method, params })),
          ),
        { method, params },
      );
      assert.equal(response.error, undefined, `${method}: ${JSON.stringify(response.error)}`);
      return response.result;
    };
    await enterWorkbench(page);
    const opened = application.waitForEvent("window");
    await page.getByRole("button", { name: "设置", exact: true }).click();
    const settings = await opened;
    settings.on("pageerror", error => errors.push(String(error)));
    await settings.getByRole("button", { name: "插件", exact: true }).click();
    const manager = settings.getByRole("region", { name: "原生插件管理", exact: true });
    await expect(manager.getByRole("table", { name: "原生插件目录" }).getByRole("row")).toHaveCount(
      3,
    );
    const inventory = (await call("native.plugins.inspect")).native_plugins.items;
    assert.equal(inventory.filter(item => item.state === "invalid").length, 1);
    const hash = inventory.find(item => item.id === "test.independent.c").sha256;
    // Providers belong to Data. Task schedules workers from the pinned authorization.
    const editor = manager.getByRole("region", { name: "服务插件 historical-data", exact: true });
    await call("node.action", { id: "local", service: "historical-data", action: "stop" });
    await editor.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }).check();
    await editor.getByRole("button", { name: "保存插件配置", exact: true }).click();
    const service = (state, id) =>
      state.nodes.find(node => node.id === "local").health.services.find(item => item.id === id);
    await expect
      .poll(async () => service(await call("runtime.snapshot"), "historical-data").plugin_artifacts)
      .toEqual([hash]);
    await call("node.research.local.open");
    await expect
      .poll(async () => (await call("runtime.snapshot")).data?.sources.map(item => item.id))
      .toEqual(["fixture.minutes"]);
    assert.deepEqual(service(await call("runtime.snapshot"), "research").plugin_artifacts, []);
    // This value is a test fixture token, never a real provider credential.
    const credential = "fixture-session-secret";
    await call("data.credentials.save", {
      provider: "test.independent.c",
      credential,
      remember: false,
      requests_per_minute: 30,
    });
    // Downloads share this provider/account budget owned by Data.
    await call("data.download.budget.configure", {
      source: "fixture.minutes",
      token: "",
      requests_per_minute: 30,
    });
    const tasks = state => state.data.tasks;
    const succeeded = async id =>
      await expect
        .poll(
          async () => {
            const task = tasks(await call("runtime.snapshot")).find(item => item.id === id);
            if (task?.state === "failed") throw new Error(task.error);
            return task?.state;
          },
          { timeout: 30000 },
        )
        .toBe("succeeded");
    // A download picks its contract from its own data source's contract list,
    // which a table update stores with the days it depends on.
    await call("data.tables.update.submit", {
      id: "native-tables",
      provider: "test.independent.c",
      tables: ["futures.daily"],
      begin_day: "2024-03-04",
      end_day: "2024-03-04",
      requests_per_minute: 30,
      token: "",
    });
    await succeeded("native-tables");
    const { snapshot } = (await call("data.tables.coverage")).table_coverage;
    const id = "native-minutes";
    const source = "fixture.minutes";
    const listed = (
      await call("data.tables.contracts", { snapshot, source, exchange: "SHFE", product: "cu" })
    ).table_contracts;
    assert.deepEqual(
      listed.items.map(item => item.code),
      ["SHFE/cu/2024-03"],
    );
    await call("data.download.minutes.submit", {
      id,
      source,
      contract_id: "SHFE/cu/2024-03",
      snapshot,
      cutoff_ns: listed.cutoff_ns,
      requests_per_minute: 30,
      token: "",
      interval_minutes: 1,
    });
    await succeeded(id);
    const completed = tasks(await call("runtime.snapshot")).find(item => item.id === id);
    assert.equal(completed.provider_artifact, hash);
    const table = (
      await call("data.tables.page", {
        table: "futures.daily",
        source: "fixture.daily.v1",
        exchange: "SHFE",
        period: "2024-03-04",
        offset: 0,
        limit: 10,
      })
    ).table_page;
    assert.equal(table.rows[0][table.columns.indexOf("close")], "72000.5");
    const current = await call("runtime.snapshot");
    assert.ok(!JSON.stringify(current).includes(credential));
    const taskDirectory = path.join(
      service(current, "historical-data").directory,
      "acquisition-tasks",
    );
    await assert.rejects(fs.access(path.join(taskDirectory, id, "provider.credential")), {
      code: "ENOENT",
    });
    const filter = { venue: "", product: "", contract_id: "", source: "" };
    const archive = (await call("data.datasets", filter)).history_datasets;
    assert.equal(archive.length, 1);
    const minute = archive[0];
    const readMinutes = async () =>
      (
        await call("data.minutes.page", {
          id: minute.id,
          offset: 0,
          limit: 10,
          start: "",
          end: "",
          include_macd: false,
        })
      ).history_page;
    assert.equal((await readMinutes()).bars[0].close, "2");
    await call("node.action", { id: "local", service: "research", action: "stop" });
    assert.equal((await readMinutes()).id, minute.id);
    await call("node.action", { id: "local", service: "historical-data", action: "stop" });
    // Removing the upload cannot remove the service's or submitted task's pinned bytes.
    await fs.unlink(original);
    await application.close();
    application = undefined;
    process.kill(Number(await fs.readFile(path.join(temp, "node/agent.pid"), "utf8")), "SIGTERM");
    await new Promise(resolve => setTimeout(resolve, 2000));
    application = await launch();
    page = await application.firstWindow();
    page.on("pageerror", error => errors.push(String(error)));
    try {
      await enterWorkbench(page);
    } catch (error) {
      console.error("Restored startup:", await page.locator("body").innerText());
      console.error("Restored services:", JSON.stringify((await call("runtime.snapshot")).nodes));
      throw error;
    }
    await call("node.data.local.open");
    await expect.poll(async () => (await call("runtime.snapshot")).data?.sources.length).toBe(1);
    const restored = await call("runtime.snapshot");
    assert.deepEqual(service(restored, "historical-data").plugin_artifacts, [hash]);
    const task = tasks(restored).find(item => item.id === id);
    assert.equal(task.state, "succeeded");
    assert.equal(task.provider_artifact, hash);
    assert.equal(task.history_dataset_id, completed.history_dataset_id);
    assert.deepEqual((await call("data.datasets", filter)).history_datasets, archive);
    assert.equal((await readMinutes()).bars[0].close, "2");
    assert.deepEqual(errors, []);
    console.log(
      "Native third-party plugin: Data discovery, managed minute and table workers, independent archive and pinned restart passed",
    );
  } finally {
    if (application) await application.close();
    try {
      process.kill(Number(await fs.readFile(path.join(temp, "node/agent.pid"), "utf8")), "SIGTERM");
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
