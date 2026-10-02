// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
const build = path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug");
(async () => {
  assert.equal(process.platform, "darwin");
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-plugin-native-"));
  let application;
  try {
    // The third-party plugin is a user-installed, optional one; plugins shipped
    // with the application are required and cannot be switched off.
    const plugins = path.join(temp, "plugins");
    const installedPlugins = path.join(temp, "node/plugins");
    await fs.mkdir(plugins);
    await fs.mkdir(installedPlugins, { recursive: true });
    await fs.copyFile(
      path.join(build, "tests/native-plugin/good/plugin_fixture_good.dylib"),
      path.join(installedPlugins, "fixture.dylib"),
    );
    await fs.copyFile(
      path.join(build, "tests/native-plugin/bad_abi/plugin_fixture_bad_abi.dylib"),
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
    await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 60000 });
    const opened = application.waitForEvent("window");
    await page.getByRole("button", { name: "设置", exact: true }).click();
    const settings = await opened;
    settings.on("pageerror", error => errors.push(String(error)));
    await settings.getByRole("button", { name: "插件", exact: true }).click();
    const manager = settings.getByRole("region", { name: "原生插件管理", exact: true });
    await expect(manager.getByRole("table", { name: "原生插件目录" }).getByRole("row")).toHaveCount(
      3,
    );
    await expect(manager.getByRole("button", { name: "刷新插件目录", exact: true })).toBeEnabled();
    const inventory = (await call("runtime.snapshot")).native_plugins.items;
    assert.equal(inventory.filter(item => item.state === "available").length, 1);
    assert.equal(inventory.filter(item => item.state === "invalid").length, 1);
    let fixtureHash = inventory.find(item => item.id === "test.independent.c").sha256;
    // An installed plugin starts disabled: enable it on the stopped service.
    const fixtureChoice = manager
      .getByRole("region", { name: "服务插件 research", exact: true })
      .getByRole("checkbox", { name: "test.independent.c · 1.0.0", exact: true });
    await expect(fixtureChoice).not.toBeChecked();
    await call("node.action", { id: "local", service: "research", action: "stop" });
    await fixtureChoice.check();
    await manager
      .getByRole("region", { name: "服务插件 research", exact: true })
      .getByRole("button", { name: "保存插件配置", exact: true })
      .click();
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).nodes
            .find(node => node.id === "local")
            .health.services.find(service => service.id === "research").plugin_artifacts,
      )
      .toEqual([fixtureHash]);
    await call("research.local");
    await expect
      .poll(async () => (await call("runtime.snapshot")).research?.sources.length)
      .toBe(2);
    const initial = await call("runtime.snapshot");
    assert.deepEqual(
      initial.research.sources.map(x => x.id),
      ["fixture.minutes", "fixture.daily"],
    );
    await settings.getByRole("button", { name: "数据源", exact: true }).click();
    // One card per provider: the fixture plugin's two sources share one credential.
    const provider = "test.independent.c";
    const card = settings.getByRole("region", { name: "数据源", exact: true }).getByRole("region", {
      name: "Third-party test minutes / Third-party test daily",
      exact: true,
    });
    await expect(card).toBeVisible();
    await expect(card.getByText("未设置", { exact: true })).toBeVisible();
    await settings.screenshot({ path: "build/data-sources-before.png", fullPage: true });
    assert.deepEqual(errors, []);
    const dialog = settings.getByRole("dialog");
    await card.getByRole("button", { name: "设置 测试凭据", exact: true }).click();
    await dialog.getByLabel("测试凭据", { exact: true }).fill("invalid");
    await dialog.getByRole("button", { name: "保存并验证", exact: true }).click();
    // Saving verifies at once: both sources report the rejected credential.
    await expect(card.locator("dd.bad").filter({ hasText: "凭据无效" }))
      .toHaveCount(2)
      .catch(async error => {
        console.error("Data source page:", await settings.locator("body").innerText());
        throw error;
      });
    await card.getByRole("button", { name: "设置 测试凭据", exact: true }).click();
    await dialog.getByLabel("测试凭据", { exact: true }).fill("fixture-session-secret");
    await dialog.getByRole("button", { name: "保存并验证", exact: true }).click();
    await expect(card.locator("dd.ok")).toHaveCount(4);
    await expect(card.locator("dd.bad")).toHaveCount(0);
    const savedEntry = async () =>
      (await call("runtime.snapshot")).data_credentials.find(item => item.provider === provider);
    assert.equal((await savedEntry()).credential_ready, true);
    assert.equal((await savedEntry()).remember, false);
    assert.ok(!JSON.stringify(await call("runtime.snapshot")).includes("fixture-session-secret"));
    const credentialFile = path.join(temp, "node/data-providers", provider + ".json");
    assert.ok(!(await fs.readFile(credentialFile, "utf8")).includes("fixture-session-secret"));
    await settings.screenshot({ path: "build/data-sources.png", fullPage: true });
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
    await expect(history.getByText("使用已保存的 测试凭据", { exact: true })).toBeVisible();
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
      });
      await call(method, {
        id,
        source,
        contract_id: "SHFE/cu/2024-03",
        catalog_cutoff_ns: catalog.history_contracts.cutoff_ns,
        requests_per_minute: 30,
        token: "",
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
    // Replacing the provider's credential does not touch a submitted task's
    // copy; remembering it keeps it across restarts.
    await call("research.credentials.save", {
      provider,
      credential: "fixture-saved-secret",
      remember: true,
      requests_per_minute: 30,
    });
    assert.equal(await fs.readFile(taskCredentialPath, "utf8"), "fixture-session-secret");
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
    await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
    const archiveView = page.getByRole("region", { name: "历史数据仓库", exact: true });
    await archiveView
      .locator(`[data-dataset-id="${daily.id}"]`)
      .getByRole("button", { name: "使用情况", exact: true })
      .click();
    const usageView = archiveView.getByRole("region", { name: "使用情况", exact: true });
    await expect(usageView).toContainText("找到 1 条关联记录");
    await expect(usageView.getByRole("table", { name: "数据版本关联记录" })).toContainText(
      "third-party-daily",
    );
    await expect(usageView).toContainText("下载产物");
    await page.screenshot({ path: "build/history-usage-native.png", fullPage: true });
    await usageView.getByRole("button", { name: "关闭", exact: true }).click();

    await archiveView
      .locator(`[data-dataset-id="${daily.id}"]`)
      .getByRole("button", { name: "下载后续数据", exact: true })
      .click();
    const updateView = archiveView.getByRole("region", { name: "下载后续数据", exact: true });
    await updateView.getByLabel("下载至日期").fill("2024-03-03");
    await updateView.getByRole("button", { name: "预览下载范围", exact: true }).click();
    await expect(updateView).toContainText("本次下载：2024-03-03 — 2024-03-03");
    await updateView.getByRole("button", { name: "开始下载此范围", exact: true }).click();
    await expect(updateView.getByRole("button", { name: "查看新版本", exact: true })).toBeVisible({
      timeout: 30000,
    });
    const extended = (
      await call("research.datasets", {
        venue: "",
        product: "",
        contract_id: "",
        source: "",
      })
    ).history_datasets;
    assert.equal(extended.length, 3);
    assert.deepEqual(
      extended.find(item => item.id === daily.id),
      daily,
    );
    const newDaily = extended.find(item => item.source === "fixture.daily" && item.id !== daily.id);
    assert.equal(newDaily.begin, "2024-03-03");
    assert.equal(newDaily.end, "2024-03-03");
    await page.screenshot({ path: "build/history-update-native.png", fullPage: true });
    await updateView.getByRole("button", { name: "查看新版本", exact: true }).click();
    await expect(
      archiveView.getByRole("region", { name: "日线数据表", exact: true }).getByRole("table"),
    ).toContainText("2024-03-03");

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
    const detachedUsage = (await call("research.history.usage", { id: daily.id })).history_usage;
    assert.deepEqual(
      detachedUsage.references.map(row => [row.kind, row.id, row.roles]),
      [["download", "third-party-daily", ["output"]]],
    );

    assert.equal(
      (await call("research.datasets", { venue: "", product: "", contract_id: "", source: "" }))
        .history_datasets.length,
      3,
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
    });
    await call("research.minutes.submit", {
      id: "pinned-retry",
      source: "fixture.minutes",
      contract_id: "SHFE/cu/2024-03",
      catalog_cutoff_ns: retryCatalog.history_contracts.cutoff_ns,
      requests_per_minute: 1,
      interval_minutes: 1,
      token: "",
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
    const newerPath = path.join(build, "tests/native-plugin/newer/plugin_fixture_newer.dylib");
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
    await manager
      .getByRole("row")
      .filter({ hasText: "2.0.0" })
      .getByRole("button", { name: "卸载插件", exact: true })
      .click();
    await manager.getByRole("button", { name: "确认卸载插件", exact: true }).click();
    await expect
      .poll(async () =>
        (await call("runtime.snapshot")).native_plugins.items.some(
          item => item.sha256 === fixtureHash,
        ),
      )
      .toBe(false);
    // The installed service pins its plugin artifacts; the original upload folder is not needed.
    await fs.unlink(path.join(installedPlugins, "fixture.dylib"));
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
    // Keep the restored workbench away from pages that prepare research automatically.
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "自选", exact: true })
      .click();
    await call("node.action", { id: "local", service: "research", action: "stop" });
    await application.close();
    application = undefined;
    process.kill(Number(await fs.readFile(path.join(temp, "node/agent.pid"), "utf8")), "SIGTERM");
    await new Promise(resolve => setTimeout(resolve, 2000));
    // The data service is stopped here, so its task index can be read.
    const index = new (require("node:sqlite").DatabaseSync)(
      path.join(temp, "node/services/research/ledger/tasks.sqlite"),
      { readOnly: true },
    );
    const originalTaskManifest = JSON.parse(
      index.prepare("SELECT manifest FROM tasks WHERE id = ?").get("third-party-minutes").manifest,
    );
    index.close();
    assert.equal(originalTaskManifest.version, 4);
    assert.equal(
      originalTaskManifest.provider_artifact,
      inventory.find(item => item.id === "test.independent.c").sha256,
    );
    assert.notEqual(originalTaskManifest.provider_artifact, fixtureHash);
    application = await launch();
    page = await application.firstWindow();
    page.on("pageerror", error => errors.push(String(error)));
    await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 60000 });
    await expect(
      page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }),
    ).toBeVisible();
    assert.equal((await savedEntry()).credential_ready, true);
    // The remembered token lives in the login keychain; remove the test entry.
    await call("research.credentials.clear", { provider });
    assert.equal(await savedEntry(), undefined);
    const restored = managed(await call("runtime.snapshot"));
    // Startup started the stopped service again, with its pinned plugins.
    assert.equal(restored.desired_running, true);
    assert.deepEqual(restored.plugin_artifacts, [fixtureHash]);
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
