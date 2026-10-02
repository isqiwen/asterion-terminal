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
  const pluginDirectory = path.resolve("build/electron-resources/native/plugins");
  let application, remoteServer;
  try {
    application = await electron.launch({
      executablePath: require("electron"),
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
        env: {
          ...process.env,
          ASTERION_NODE_DIRECTORY: root,
          ASTERION_TEST_NODE_ISOLATED: "1",
          ASTERION_PLUGIN_DIRECTORY: pluginDirectory,
        },
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
        env: {
          ...process.env,
          ASTERION_NODE_DIRECTORY: root,
          ASTERION_TEST_NODE_ISOLATED: "1",
          ASTERION_PLUGIN_DIRECTORY: pluginDirectory,
        },
        timeout: 15000,
      },
    );
    const prefix = await execFile(
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", "asterion_test_history"),
      ["--directory", service.directory, "--id", "native-prefix", "--price", "100", "101", "102"],
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
    await call("research.local");
    await call("research.dataset.clear");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: "新建回测", exact: true }).click();
    const input = JSON.parse(seeded.stdout),
      extra = JSON.parse(prefix.stdout);
    const picker = page.getByRole("form", { name: "历史数据集" });
    await picker.getByLabel("K 线来源", { exact: true }).selectOption(input.source_dataset_ids[0]);
    if (await picker.getByLabel("结算价来源", { exact: true }).isVisible())
      await picker
        .getByLabel("结算价来源", { exact: true })
        .selectOption(input.settlement_dataset_ids[0]);
    await picker.getByLabel("最小变动价位", { exact: true }).fill("1");
    await picker.getByLabel("合约乘数", { exact: true }).fill("10");
    await picker.getByText("拼接更多下载", { exact: false }).click();
    await picker
      .getByRole("group", { name: "补充 K 线", exact: true })
      .getByRole("checkbox", { name: new RegExp(extra.source_dataset_ids[0].slice(0, 8)) })
      .check();
    await picker.getByRole("button", { name: "使用此数据集", exact: true }).click();
    await expect(picker.getByRole("list", { name: "已选合约" })).toContainText("7 根");
    const selected = (await call("runtime.snapshot")).datasets;
    assert.equal(selected[0].source_dataset_ids.length, 2);
    await picker.locator(".dataset-composition").scrollIntoViewIfNeeded();
    await page.screenshot({ path: "build/history-archive/native-composition-picker.png" });
    await page.getByText("保存当前选择", { exact: true }).click();
    await page.getByLabel("数据集名称", { exact: true }).fill("原生回测输入");
    await page.getByRole("button", { name: "保存数据集", exact: true }).click();
    await expect(
      page.getByText("数据集已保存，可在当前研究服务中重复使用。", { exact: true }),
    ).toBeVisible();
    const saved = (await call("research.dataset.saved")).saved_datasets[0];
    await call("research.dataset.clear");
    await page.reload();
    await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 60000 });
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: "新建回测", exact: true }).click();
    await page.getByRole("combobox", { name: "已保存数据集", exact: true }).selectOption(saved.id);
    await page.getByRole("button", { name: "使用已保存数据集", exact: true }).click();
    await expect(page.getByRole("list", { name: "已选合约" })).toContainText("7 根");
    assert.deepEqual((await call("runtime.snapshot")).datasets, selected);
    await page.screenshot({
      path: "build/history-archive/native-composed-dataset.png",
      fullPage: true,
    });
    const policy = {
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
    const binary = name => path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", name);
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
    const archiveUsage = page.getByRole("region", { name: "历史数据仓库", exact: true });
    await archiveUsage
      .locator(`[data-dataset-id="${input.source_dataset_ids[0]}"]`)
      .getByRole("button", { name: "使用情况", exact: true })
      .click();
    const usagePanel = archiveUsage.getByRole("region", { name: "使用情况", exact: true });
    const ipcId = (await fs.readFile(path.join(root, "ipc-id"), "utf8")).trim();
    await execFile(
      binary("asterion_test_node_service_control"),
      [
        "--operation",
        "deploy-research",
        "--executable",
        binary("asterion-node-agent"),
        "--root",
        root,
        "--endpoint",
        `/tmp/ast-node-${ipcId}/node.sock`,
        "--name",
        "me.asterion.acceptance.cross-research",
      ],
      {
        env: {
          ...process.env,
          ASTERION_NODE_DIRECTORY: root,
          ASTERION_TEST_NODE_ISOLATED: "1",
          ASTERION_PLUGIN_DIRECTORY: pluginDirectory,
        },
        timeout: 20000,
      },
    );
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).nodes
            .find(n => n.id === "local")
            .health.services.find(s => s.id === "other-research")?.health,
        { timeout: 15000 },
      )
      .toBe("ready");
    // The baseline must already show the fixture's stopped service as stopped.
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).nodes
            .find(n => n.id === "local")
            .health.services.find(s => s.id === "stopped-research")?.state,
        { timeout: 15000 },
      )
      .toBe("stopped");
    const disconnectedDirectory = path.join(root, "enrollments", "native-offline-fixture");
    await fs.mkdir(disconnectedDirectory, { recursive: true });
    await fs.writeFile(
      path.join(disconnectedDirectory, "enrollment.json"),
      "intentionally unread: test fixture",
    );
    const beforeCross = await call("runtime.snapshot");
    await usagePanel.getByRole("button", { name: "刷新使用情况", exact: true }).click();
    const otherResearch = usagePanel.getByRole("region", { name: "其他研究服务", exact: true });
    await expect(
      otherResearch.getByRole("region", { name: "本机 / other-research", exact: true }),
    ).toContainText("已检查，发现 0 条关联记录");
    const stoppedResearch = otherResearch.getByRole("region", {
      name: "本机 / stopped-research",
      exact: true,
    });
    await expect(stoppedResearch.getByRole("alert")).toHaveCount(0);
    await expect(stoppedResearch).toContainText("已检查，发现 0 条关联记录");
    await expect(stoppedResearch).toContainText("本机账本已检查；服务保持停止。");
    await expect(
      usagePanel.getByRole("region", { name: "未连接的节点", exact: true }),
    ).toContainText("native-offline-fixture");
    const afterCross = await call("runtime.snapshot");
    assert.ok(!afterCross.nodes.some(n => n.id === "native-offline-fixture"));
    assert.equal(afterCross.research.connection_id, beforeCross.research.connection_id);
    assert.deepEqual(afterCross.datasets, beforeCross.datasets);
    const nodeServices = afterCross.nodes.find(n => n.id === "local").health.services;
    assert.equal(nodeServices.find(s => s.id === "stopped-research").state, "stopped");
    const researchState = snapshot =>
      snapshot.nodes
        .find(n => n.id === "local")
        .health.services.filter(s =>
          ["research", "other-research", "stopped-research"].includes(s.id),
        )
        .map(s => ({
          id: s.id,
          pid: s.pid,
          state: s.state,
          restarts: s.restarts,
          desired_running: s.desired_running,
        }));
    assert.deepEqual(researchState(afterCross), researchState(beforeCross));
    await fs.writeFile(
      "build/history-archive/native-cross-research-state.json",
      JSON.stringify(
        {
          before: researchState(beforeCross),
          after: researchState(afterCross),
          same_connection: afterCross.research.connection_id === beforeCross.research.connection_id,
          same_datasets:
            JSON.stringify(afterCross.datasets) === JSON.stringify(beforeCross.datasets),
        },
        null,
        2,
      ),
    );
    await otherResearch.scrollIntoViewIfNeeded();
    await page.screenshot({
      path: "build/history-archive/native-cross-research-usage.png",
      fullPage: true,
    });
    console.log(
      "Native cross-research usage: running and stopped ledgers, offline inventory, unchanged active workspace passed",
    );

    await call("node.action", { id: "local", service: "research", action: "stop" });
    const stoppedTaskDigest = await digest(path.join(service.directory, "tasks.sqlite"));
    await call("research.attach", { id: "local", service: "other-research" });
    const stoppedReferences = (
      await call("research.history.usage", { id: input.source_dataset_ids[0] })
    ).history_usage.other_research.find(s => s.node === "local" && s.service === "research");
    assert.equal(stoppedReferences.checked, true);
    assert.equal(stoppedReferences.stopped, true);
    assert.ok(
      stoppedReferences.references.some(
        r => r.id === "native-risk-backtest" && r.kind === "backtest",
      ),
    );
    assert.ok(stoppedReferences.references.some(r => r.kind === "saved_dataset"));
    assert.equal(await digest(path.join(service.directory, "tasks.sqlite")), stoppedTaskDigest);
    const stoppedState = (await call("runtime.snapshot")).nodes
      .find(n => n.id === "local")
      .health.services.find(s => s.id === "research");
    assert.equal(stoppedState.state, "stopped");
    assert.equal(stoppedState.pid, 0);
    await fs.writeFile(
      "build/history-archive/native-stopped-research-usage.json",
      JSON.stringify(
        {
          usage: stoppedReferences,
          ledger_unchanged: true,
          state: stoppedState.state,
          pid: stoppedState.pid,
        },
        null,
        2,
      ),
    );
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
    if (remoteServer && remoteServer.exitCode === null) {
      await new Promise(resolve => {
        remoteServer.once("exit", resolve);
        remoteServer.kill("SIGTERM");
      });
    }
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
