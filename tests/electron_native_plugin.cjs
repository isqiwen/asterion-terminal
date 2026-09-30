// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
(async () => {
  assert.equal(process.platform, "darwin");
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-plugin-native-"));
  let application;
  try {
    const plugins = path.join(temp, "plugins");
    await fs.mkdir(plugins);
    await fs.copyFile(
      path.resolve("build/Debug/tests/native-plugin/good/plugin_fixture_good.dylib"),
      path.join(plugins, "fixture.dylib"),
    );
    await fs.copyFile(
      path.resolve("build/Debug/plugins/asterion-tushare.dylib"),
      path.join(plugins, "tushare.dylib"),
    );
    await fs.copyFile(
      path.resolve("build/Debug/tests/native-plugin/bad_abi/plugin_fixture_bad_abi.dylib"),
      path.join(plugins, "broken.dylib"),
    );
    const launch = () =>
      electron.launch({
        executablePath: require("electron"),
        args: [path.resolve("apps/clients/terminal/electron"), `--user-data-dir=${temp}/ui`],
        env: {
          ...process.env,
          ASTERION_NODE_DIRECTORY: `${temp}/node`,
          ASTERION_TEST_NODE_ISOLATED: "1",
          ASTERION_PLUGIN_DIRECTORY: plugins,
        },
        timeout: 30000,
      });
    application = await launch();
    let page = await application.firstWindow();
    const errors = [];
    page.on("pageerror", error => errors.push(String(error)));
    await page.waitForFunction(() => !!window.asterionDesktop);
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
    await page
      .getByRole("checkbox", { name: "asterion.data.tushare · 1.0.0", exact: true })
      .uncheck();
    await expect(
      page.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }),
    ).toBeChecked();
    await page.getByRole("button", { name: "开始设置", exact: true }).click();
    await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 60000 });
    const opened = application.waitForEvent("window");
    await page.getByRole("button", { name: "设置", exact: true }).click();
    const settings = await opened;
    settings.on("pageerror", error => errors.push(String(error)));
    await settings.getByRole("button", { name: "插件", exact: true }).click();
    const manager = settings.getByRole("region", { name: "原生插件管理", exact: true });
    await expect(manager.getByRole("table", { name: "原生插件目录" }).getByRole("row")).toHaveCount(
      4,
    );
    await expect(manager.getByRole("button", { name: "刷新插件目录", exact: true })).toBeEnabled();
    const inventory = (await call("runtime.snapshot")).native_plugins.items;
    assert.equal(inventory.filter(item => item.state === "available").length, 2);
    assert.equal(inventory.filter(item => item.state === "invalid").length, 1);
    let fixtureHash = inventory.find(item => item.id === "test.independent.c").sha256;
    await expect(
      manager.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }),
    ).toBeChecked();
    await expect(
      manager.getByRole("checkbox", { name: "asterion.data.tushare · 1.0.0", exact: true }),
    ).not.toBeChecked();
    await expect
      .poll(async () => (await call("runtime.snapshot")).research?.sources.length)
      .toBe(2);
    const initial = await call("runtime.snapshot");
    assert.deepEqual(
      initial.research.sources.map(x => x.id),
      ["fixture.minutes", "fixture.daily"],
    );
    await settings.getByRole("button", { name: "连接与部署", exact: true }).click();
    const connections = settings.getByRole("region", { name: "数据源连接", exact: true });
    await expect(connections).toBeVisible();
    await settings.screenshot({ path: "build/data-connections-before.png", fullPage: true });
    assert.deepEqual(errors, []);
    await connections.getByLabel("数据源", { exact: true }).selectOption("fixture.minutes");
    await connections.getByLabel("连接名称", { exact: true }).fill("测试会话连接");
    await connections.getByLabel("测试凭据", { exact: true }).fill("invalid");
    await connections.getByRole("button", { name: "保存连接", exact: true }).click();
    const savedRegion = connections.getByRole("region", { name: "测试会话连接", exact: true });
    await savedRegion.getByRole("button", { name: "验证连接", exact: true }).click();
    await expect(savedRegion.getByText("合约目录：凭据无效", { exact: true })).toBeVisible();
    await savedRegion.getByRole("button", { name: "编辑连接", exact: true }).click();
    await connections.getByLabel("测试凭据", { exact: true }).fill("fixture-session-secret");
    await connections.getByRole("button", { name: "保存连接", exact: true }).click();
    await savedRegion.getByRole("button", { name: "验证连接", exact: true }).click();
    await expect(
      savedRegion.getByText("历史数据接口：接口访问成功", { exact: true }),
    ).toBeVisible();
    let profile = (await call("runtime.snapshot")).data_connections[0];
    assert.equal(profile.credential_ready, true);
    assert.ok(!JSON.stringify(await call("runtime.snapshot")).includes("fixture-session-secret"));
    const connectionFile = path.join(temp, "node/data-connections", profile.id + ".json");
    assert.ok(!(await fs.readFile(connectionFile, "utf8")).includes("fixture-session-secret"));
    await call("research.connections.save", {
      id: "remembered",
      name: "Saved token",
      source: "fixture.daily",
      revision: "",
      requests_per_minute: 30,
      remember: true,
      credential: "fixture-saved-secret",
      credential_action: "replace",
    });
    await connections.scrollIntoViewIfNeeded();
    await settings.screenshot({ path: "build/data-connections.png", fullPage: true });
    await settings.getByRole("button", { name: "插件", exact: true }).click();
    await page
      .getByRole("navigation", { name: "业务工作区" })
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page.getByRole("button", { name: "历史数据", exact: true }).click();
    const history = page.getByRole("region", { name: "历史数据", exact: true });
    await expect(history.getByLabel("数据源", { exact: true }).locator("option")).toHaveText([
      "Third-party test minutes · 分钟 K 线",
      "Third-party test daily · 日 K 线",
    ]);
    await history.getByLabel("品种代码", { exact: true }).fill("CU");
    await history.getByLabel("使用连接", { exact: true }).selectOption(profile.id);
    await expect(history.getByRole("button", { name: "查询月份合约", exact: true })).toBeEnabled();
    await history.getByRole("button", { name: "查询月份合约", exact: true }).click();
    await expect
      .poll(async () => {
        const alerts = await page.getByRole("alert").allTextContents();
        if (alerts.length) throw new Error(alerts.join(" | "));
        return (await call("runtime.snapshot")).history_contracts?.items.length;
      })
      .toBe(1);
    for (const [source, method, id, extra] of [
      [
        "fixture.minutes",
        "research.minutes.submit",
        "third-party-minutes",
        { interval_minutes: 1 },
      ],
      ["fixture.daily", "research.daily.submit", "third-party-daily", {}],
    ]) {
      const catalog = await call("research.contracts.load", {
        source,
        exchange: "SHFE",
        product: "CU",
        token: "",
        connection: source === "fixture.minutes" ? profile.id : "",
        connection_revision: source === "fixture.minutes" ? profile.revision : "",
      });
      await call(method, {
        id,
        source,
        contract_id: "SHFE/cu/2024-03",
        catalog_cutoff_ns: catalog.history_contracts.cutoff_ns,
        requests_per_minute: source === "fixture.minutes" ? profile.requests_per_minute : 60,
        token: "",
        connection: source === "fixture.minutes" ? profile.id : "",
        connection_revision: source === "fixture.minutes" ? profile.revision : "",
        ...extra,
      });
      await expect
        .poll(
          async () => {
            const task = (await call("runtime.snapshot")).research.tasks.find(t => t.id === id);
            if (task?.state === "failed") throw new Error(task.error);
            return task?.state;
          },
          { timeout: 30000 },
        )
        .toBe("succeeded");
    }
    const taskCredentialPath = path.join(
      temp,
      "node/services/research/ledger/third-party-minutes/provider.credential",
    );
    assert.equal(await fs.readFile(taskCredentialPath, "utf8"), "fixture-session-secret");
    await call("research.connections.save", {
      id: profile.id,
      name: profile.name,
      source: profile.source,
      revision: profile.revision,
      requests_per_minute: profile.requests_per_minute,
      remember: false,
      credential: "replacement-session-secret",
      credential_action: "replace",
    });
    const changedProfile = (await call("runtime.snapshot")).data_connections.find(
      item => item.id === profile.id,
    );
    assert.notEqual(changedProfile.revision, profile.revision);
    const staleProfile = await page.evaluate(
      async old =>
        JSON.parse(
          await window.asterionDesktop.request(
            JSON.stringify({
              version: 1,
              method: "research.connections.verify",
              params: { id: old.id, revision: old.revision, source: old.source },
            }),
          ),
        ),
      profile,
    );
    assert.equal(staleProfile.error.code, "conflict");
    assert.equal(await fs.readFile(taskCredentialPath, "utf8"), "fixture-session-secret");
    profile = changedProfile;
    const archive = await call("research.datasets", {
      venue: "",
      product: "",
      contract_id: "",
      source: "",
    });
    assert.equal(archive.history_datasets.length, 2);
    const daily = archive.history_datasets.find(x => x.source === "fixture.daily");
    const pageResult = await call("research.daily.page", {
      id: daily.id,
      archive: true,
      offset: 0,
      limit: 10,
      start: "",
      end: "",
      include_macd: false,
      period: "day",
    });
    assert.equal(pageResult.daily_page.bars[0].close, "2");
    const configuration = JSON.parse(
      await fs.readFile(path.join(temp, "node/services/research/service.json"), "utf8"),
    );
    assert.equal(configuration.version, 3);
    assert.equal(configuration.plugin_artifacts.length, 1);
    const managed = snapshot =>
      snapshot.nodes
        .find(node => node.id === "local")
        .health.services.find(service => service.id === "research");
    const reject = async (params, diagnostic) => {
      const response = await page.evaluate(
        async params =>
          JSON.parse(
            await window.asterionDesktop.request(
              JSON.stringify({ version: 1, method: "node.plugins.configure", params }),
            ),
          ),
        params,
      );
      assert.ok(response.error?.message.includes(diagnostic), JSON.stringify(response));
    };
    const running = managed(await call("runtime.snapshot"));
    await reject(
      { id: "local", service: "research", revision: running.revision, plugins: [] },
      "stop the service",
    );
    const editor = manager.getByRole("region", { name: "服务插件 research", exact: true });
    await editor.getByRole("button", { name: "停止", exact: true }).click();
    await expect(
      editor.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }),
    ).toBeEnabled();
    const stopped = managed(await call("runtime.snapshot"));
    await reject(
      { id: "local", service: "research", revision: stopped.revision, plugins: ["0".repeat(64)] },
      "catalog changed",
    );
    await editor
      .getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true })
      .uncheck();
    await editor.getByRole("button", { name: "保存插件配置", exact: true }).click();
    await expect
      .poll(async () => managed(await call("runtime.snapshot")).plugin_artifacts.length)
      .toBe(0);
    await reject(
      { id: "local", service: "research", revision: stopped.revision, plugins: [fixtureHash] },
      "configuration changed",
    );
    let current = managed(await call("runtime.snapshot"));
    assert.equal(current.desired_running, false);
    await call("node.update", { id: "local", service: "research", revision: current.revision });
    assert.deepEqual(managed(await call("runtime.snapshot")).plugin_artifacts, []);
    const emptyConfig = JSON.parse(
      await fs.readFile(path.join(temp, "node/services/research/service.json"), "utf8"),
    );
    assert.equal(emptyConfig.desired, false);
    assert.deepEqual(emptyConfig.plugin_artifacts, []);
    await call("research.local");
    assert.deepEqual((await call("runtime.snapshot")).research.sources, []);
    assert.equal(
      (await call("research.datasets", { venue: "", product: "", contract_id: "", source: "" }))
        .history_datasets.length,
      2,
    );
    await call("node.action", { id: "local", service: "research", action: "stop" });
    await expect(
      editor.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }),
    ).toBeEnabled();
    await editor.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }).check();
    await editor.getByRole("button", { name: "保存插件配置", exact: true }).click();
    await expect
      .poll(async () => managed(await call("runtime.snapshot")).plugin_artifacts)
      .toEqual([fixtureHash]);
    // Leave an old-version task partially downloaded before changing the service version.
    await call("research.local");
    const retryCatalog = await call("research.contracts.load", {
      source: "fixture.minutes",
      exchange: "SHFE",
      product: "CU",
      token: "",
      connection: "",
      connection_revision: "",
    });
    await call("research.minutes.submit", {
      id: "pinned-retry",
      source: "fixture.minutes",
      contract_id: "SHFE/cu/2024-03",
      catalog_cutoff_ns: retryCatalog.history_contracts.cutoff_ns,
      requests_per_minute: 1,
      interval_minutes: 1,
      token: "",
      connection: "",
      connection_revision: "",
    });
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).research.tasks.find(item => item.id === "pinned-retry")
            ?.completed,
        { timeout: 15000 },
      )
      .toBe(1);
    await call("research.action", { id: "pinned-retry", action: "cancel" });
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).research.tasks.find(item => item.id === "pinned-retry")
            ?.state,
        { timeout: 15000 },
      )
      .toBe("cancelled");
    await call("node.action", { id: "local", service: "research", action: "stop" });
    await expect(
      editor.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }),
    ).toBeEnabled();
    const newerPath = path.resolve(
      "build/Debug/tests/native-plugin/newer/plugin_fixture_newer.dylib",
    );
    await application.evaluate(({ dialog }, file) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [file] });
    }, newerPath);
    await manager
      .getByRole("checkbox", {
        name: "我信任此动态库的来源；读取插件信息会执行其本机代码。",
        exact: true,
      })
      .check();
    await manager.getByRole("button", { name: "选择插件动态库", exact: true }).click();
    await expect(manager.getByRole("button", { name: "确认安装插件", exact: true })).toBeVisible();
    await manager.getByRole("button", { name: "确认安装插件", exact: true }).click();
    await expect(manager.getByRole("button", { name: "确认安装插件", exact: true })).toBeHidden();
    const installed = (await call("runtime.snapshot")).native_plugins.items.find(
      item => item.version === "2.0.0",
    );
    assert.equal(installed.managed, true);
    await editor.getByRole("checkbox", { name: "test.independent.c · 2.0.0", exact: true }).check();
    await expect(
      editor.getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true }),
    ).not.toBeChecked();
    await editor.getByRole("button", { name: "保存插件配置", exact: true }).click();
    fixtureHash = installed.sha256;
    await expect
      .poll(async () => managed(await call("runtime.snapshot")).plugin_artifacts)
      .toEqual([fixtureHash]);
    await manager.getByRole("button", { name: "卸载插件", exact: true }).click();
    await manager.getByRole("button", { name: "确认卸载插件", exact: true }).click();
    await expect
      .poll(async () =>
        (await call("runtime.snapshot")).native_plugins.items.some(
          item => item.sha256 === fixtureHash,
        ),
      )
      .toBe(false);
    // The installed service pins its plugin artifacts; the original upload folder is not needed.
    await fs.unlink(path.join(plugins, "fixture.dylib"));
    await call("research.local");
    assert.equal(
      (await call("runtime.snapshot")).research.sources[0].plugin_id,
      "test.independent.c",
    );
    await call("research.action", { id: "pinned-retry", action: "retry" });
    await expect
      .poll(
        async () => {
          const task = (await call("runtime.snapshot")).research.tasks.find(
            item => item.id === "pinned-retry",
          );
          if (task?.state === "failed") throw new Error(task.error);
          return task?.state;
        },
        { timeout: 15000 },
      )
      .toBe("succeeded");
    assert.equal(
      (await call("runtime.snapshot")).research.tasks.find(item => item.id === "pinned-retry")
        .provider_artifact,
      inventory.find(item => item.id === "test.independent.c").sha256,
    );
    assert.deepEqual(errors, []);
    await expect(editor.getByRole("button", { name: "停止", exact: true })).toBeEnabled();
    await manager.getByRole("heading").first().scrollIntoViewIfNeeded();
    await settings.screenshot({ path: "build/native-plugin-management.png", fullPage: true });
    await call("node.action", { id: "local", service: "research", action: "stop" });
    await application.close();
    application = undefined;
    process.kill(Number(await fs.readFile(path.join(temp, "node/agent.pid"), "utf8")), "SIGTERM");
    await new Promise(resolve => setTimeout(resolve, 2000));
    application = await launch();
    page = await application.firstWindow();
    page.on("pageerror", error => errors.push(String(error)));
    await expect(page.getByRole("button", { name: "总览", exact: true })).toBeVisible({
      timeout: 60000,
    });
    const originalTaskManifest = JSON.parse(
      await fs.readFile(
        path.join(temp, "node/services/research/ledger/third-party-minutes/journal/00000000.json"),
        "utf8",
      ),
    );
    assert.equal(originalTaskManifest.version, 3);
    assert.equal(
      originalTaskManifest.provider_artifact,
      inventory.find(item => item.id === "test.independent.c").sha256,
    );
    assert.notEqual(originalTaskManifest.provider_artifact, fixtureHash);
    const restoredProfiles = (await call("runtime.snapshot")).data_connections;
    assert.equal(restoredProfiles.find(item => item.id === profile.id).credential_ready, false);
    assert.equal(restoredProfiles.find(item => item.id === "remembered").credential_ready, true);
    const restored = managed(await call("runtime.snapshot"));
    assert.equal(restored.desired_running, false);
    assert.deepEqual(restored.plugin_artifacts, [fixtureHash]);
    await call("research.local");
    assert.equal(
      (await call("runtime.snapshot")).research.sources[0].plugin_id,
      "test.independent.c",
    );
    assert.deepEqual(errors, []);
    console.log(
      "Native third-party plugin: discovery, generic UI, managed minute/daily workers, archive and pinned restart passed",
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
