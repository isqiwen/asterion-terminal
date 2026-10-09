// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const { enterWorkbench, testNode } = require("./electron_startup.cjs");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const execFile = require("node:util").promisify(require("node:child_process").execFile);
async function closeDesktop(application) {
  let timer;
  try {
    await Promise.race([
      application.close(),
      new Promise((_, reject) => {
        timer = setTimeout(
          () => reject(new Error("Electron did not release its process channels")),
          15000,
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}
(async () => {
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-electron-"));
  let application;
  const capture = async (page, name) => {
    if (!process.env.ASTERION_UI_SCREENSHOTS) return;
    await fs.mkdir(process.env.ASTERION_UI_SCREENSHOTS, { recursive: true });
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.mouse.move(20, 20);
    const window = await application.browserWindow(page);
    // Capture the physical window; CDP screenshots crop Electron pages at native zoom.
    const pixels = await window.evaluate(async win =>
      (await win.webContents.capturePage()).toPNG().toString("base64"),
    );
    await fs.writeFile(
      path.join(process.env.ASTERION_UI_SCREENSHOTS, name + ".png"),
      Buffer.from(pixels, "base64"),
    );
  };
  const launch = () =>
    electron.launch({
      chromiumSandbox: true,
      executablePath:
        process.env.ASTERION_TEST_ELECTRON || require("../../scripts/desktop/electron-path.cjs")(),
      args: [
        ...(process.env.ASTERION_TEST_ELECTRON
          ? []
          : [path.resolve("apps/clients/terminal/electron")]),
        `--user-data-dir=${temp}/ui`,
      ],
      env: {
        ...process.env,
        ASTERION_NODE_DIRECTORY: `${temp}/node`,
        ...testNode,
      },
      timeout: 30000,
    });
  try {
    application = await launch();
    const page = await application.firstWindow();
    page.on("pageerror", error => console.error("Renderer error:", error));
    await page.waitForFunction(() => !!window.asterionDesktop);
    assert.equal(await page.evaluate(() => typeof require), "undefined");
    assert.equal(await page.evaluate(() => typeof window.asterionDesktop.auth), "undefined");
    assert.equal(await page.evaluate(() => window.asterionDesktop.platform), process.platform);
    const environment = await application.evaluate(({ app }) =>
      app.isPackaged ? "production" : "development",
    );
    assert.equal(await page.evaluate(() => window.asterionDesktop.environment), environment);
    assert.equal(await application.evaluate(() => process.env.ASTERION_ENVIRONMENT), environment);
    await expect(page.locator(".environment-label")).toHaveCount(
      environment === "development" ? 1 : 0,
    );

    // The page's title bar is the window's on both platforms.
    await expect(page.locator("html")).toHaveAttribute(
      "data-integrated-titlebar",
      process.platform === "darwin" ? "macos" : "overlay",
    );
    assert.equal(
      await page
        .locator(".terminal-titlebar")
        .evaluate(element => getComputedStyle(element).getPropertyValue("-webkit-app-region")),
      "drag",
    );
    assert.equal(
      await page
        .locator(".terminal-titlebar > div")
        .evaluate(element => getComputedStyle(element).getPropertyValue("-webkit-app-region")),
      "no-drag",
    );
    if (process.platform !== "darwin") {
      // Linux draws the window buttons over the bar's right end; the bar's
      // content must stop before them.
      const bar = await page.evaluate(() => {
        const free = navigator.windowControlsOverlay.getTitlebarAreaRect();
        return {
          overlay: navigator.windowControlsOverlay.visible,
          free: free.x + free.width,
          content: document
            .querySelector(".terminal-titlebar")
            .lastElementChild.getBoundingClientRect().right,
          width: innerWidth,
        };
      });
      assert.ok(bar.overlay && bar.free < bar.width, JSON.stringify(bar));
      assert.ok(bar.content <= bar.free, JSON.stringify(bar));
    }
    const reply = await page.evaluate(async () =>
      JSON.parse(
        await window.asterionDesktop.request(
          JSON.stringify({ version: 1, method: "runtime.snapshot", params: {} }),
        ),
      ),
    );
    assert.equal(reply.result.protocol, 1);
    // Startup runs by itself and opens the workbench once every step passes.
    await enterWorkbench(page);
    await capture(page, "native-startup");
    for (const [name, selector] of [
      ["自选", ".watchlist-workspace"],
      ["合约", ".contract-workspace"],
      ["市场", ".market-workspace"],
    ]) {
      await page
        .getByRole("navigation", { name: "业务工作区" })
        .getByRole("button", { name, exact: true })
        .click();
      await expect(page.locator(selector)).toBeVisible();
    }

    const workspaceNavigation = page.getByRole("navigation", { name: "业务工作区" });
    await workspaceNavigation.getByRole("button", { name: "自选", exact: true }).click();
    const priceHeading = page.locator(".watchlist-table th").filter({ hasText: /^现价$/ });
    await priceHeading.getByRole("button").click();
    await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
    await workspaceNavigation.getByRole("button", { name: "合约", exact: true }).click();
    await workspaceNavigation.getByRole("button", { name: "自选", exact: true }).click();
    await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
    await page.getByRole("button", { name: "最近浏览", exact: true }).click();
    await expect(priceHeading).not.toHaveAttribute("aria-sort");
    await expect(page.locator(".watchlist-chart-pane")).toHaveCount(0);
    await page
      .getByRole("navigation", { name: "自选视图" })
      .getByRole("button", { name: "自选", exact: true })
      .click();
    await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
    await capture(page, "native-watchlist-sort");
    await workspaceNavigation.getByRole("button", { name: "市场", exact: true }).click();

    const columnButton = page.getByRole("button", { name: "行情表头设置", exact: true });
    if (!(await page.getByRole("dialog", { name: "行情设置", exact: true }).count()))
      await page.getByRole("button", { name: "行情设置", exact: true }).click();
    await columnButton.click();
    const columnDialog = page.getByRole("dialog", { name: "行情表头设置" });
    await expect(columnDialog).toBeVisible();
    await columnDialog.getByRole("checkbox", { name: "持仓量", exact: true }).uncheck();
    await columnDialog.getByRole("button", { name: "应用列设置", exact: true }).click();
    await expect(columnButton).toBeFocused();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page
      .getByRole("navigation", { name: "业务工作区" })
      .getByRole("button", { name: "市场", exact: true })
      .click();
    if (!(await page.getByRole("dialog", { name: "行情设置", exact: true }).count()))
      await page.getByRole("button", { name: "行情设置", exact: true }).click();
    await columnButton.click();
    await expect(
      columnDialog.getByRole("checkbox", { name: "持仓量", exact: true }),
    ).not.toBeChecked();
    await capture(page, "native-quote-columns");
    await page.keyboard.press("Escape");
    await expect(columnDialog).toHaveCount(0);
    await expect(columnButton).toBeFocused();

    await require("./electron_history.cjs")(
      page,
      temp,
      capture,
      await application.evaluate(() => process.env.ASTERION_PLUGIN_DIRECTORY),
    );
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
    await call("node.data_tasks.local.open");
    const stopped = await call("node.action", {
      id: "local",
      service: "historical-data",
      action: "stop",
    });
    const service = stopped.nodes
      .find(n => n.id === "local")
      .health.services.find(s => s.id === "historical-data");
    const root = await fs.realpath(path.join(temp, "node"));
    assert.equal(
      await fs.realpath(service.directory),
      path.join(root, "services/historical-data/ledger"),
    );
    const seeded = await execFile(
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug", "asterion_test_history"),
      ["--directory", service.directory, "--id", "native-history", "--price", "100", "99", "110"],
      {
        env: { ...process.env, ASTERION_NODE_DIRECTORY: root, ...testNode },
        timeout: 15000,
      },
    );
    await call("node.data_tasks.local.open");
    const historySelection = JSON.parse(seeded.stdout);
    assert.ok(historySelection.source_dataset_ids[0]);
    await call("data.dataset.clear");
    // Fixtures change the native core outside React; establish a fresh snapshot before UI work.
    await page.reload();
    await enterWorkbench(page);
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page.getByRole("button", { name: "历史数据", exact: true }).click();
    const minuteDownloads = page.getByRole("region", { name: "历史数据", exact: true });
    await minuteDownloads.getByLabel("品种代码", { exact: true }).fill("CU");
    await minuteDownloads
      .getByLabel("Tushare Token", { exact: true })
      .fill("electron-fixture-secret");

    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: "回测", exact: true }).click();
    await page.getByRole("button", { name: "新建回测", exact: true }).click();
    const backtestData = page.getByRole("form", { name: "历史数据集", exact: true });
    await expect(backtestData).toBeVisible();
    await expect(
      backtestData.getByRole("list", { name: "已选合约", exact: true }).locator("li"),
    ).toHaveCount(0);
    await backtestData
      .getByLabel("K 线来源", { exact: true })
      .selectOption(historySelection.source_dataset_ids[0]);
    await backtestData.getByLabel("最小变动价位", { exact: true }).fill("1");
    await backtestData.getByLabel("合约乘数", { exact: true }).fill("10");
    await backtestData.getByRole("button", { name: "使用此数据集", exact: true }).click();
    await expect(backtestData.getByRole("list", { name: "已选合约", exact: true })).toContainText(
      "3 根 · 1 个交易日",
    );
    await capture(page, "native-backtest-data");
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    await page.getByLabel("初始资金", { exact: true }).fill("25000");
    await capture(page, "native-backtest-rules");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await expect(page.getByLabel("CSV 文件路径", { exact: true })).toHaveCount(0);
    await page.getByRole("button", { name: "历史数据", exact: true }).click();
    await expect(minuteDownloads.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
    await expect(minuteDownloads.getByLabel("品种代码", { exact: true })).toHaveValue("CU");

    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await expect(page.getByLabel("初始资金", { exact: true })).toHaveValue("25000");
    for (const [label, value] of [
      ["快均线", "1"],
      ["慢均线", "2"],
      ["目标手数", "1"],
      ["单笔数量上限", "1"],
      ["总持仓量上限", "1"],
      ["在途委托数上限", "1"],
      ["每手保证金", "100"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    await capture(page, "native-backtest-review");
    await page.getByRole("button", { name: "开始回测", exact: true }).click();
    await expect(page.getByRole("region", { name: "回测进度", exact: true })).toContainText(
      "已完成",
      { timeout: 20000 },
    );
    await page.getByRole("button", { name: "查看结果", exact: true }).click();
    await expect(page.getByRole("region", { name: "回测结果", exact: true })).toBeVisible();
    await page.locator(".terminal-business").evaluate(el => (el.scrollTop = 0));
    await capture(page, "native-backtest-result");

    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();

    await page.getByRole("button", { name: "历史数据", exact: true }).click();
    await expect(page.getByLabel("品种代码", { exact: true })).toHaveValue("CU");

    const modifier = process.platform === "darwin" ? "Meta" : "Control";
    await page.keyboard.press(`${modifier}+b`);
    await expect(page.locator(".activity-rail")).toHaveClass(/collapsed/);
    await page.keyboard.press(`${modifier}+b`);
    await expect(page.locator(".activity-rail")).toHaveClass(/expanded/);
    const opened = application.waitForEvent("window");
    await page.keyboard.press(`${modifier}+,`);
    await opened;
    const windows = application.windows();
    assert.equal(windows.length, 2);
    const settings = windows.find(window => window !== page);
    await settings.waitForFunction(
      () => document.querySelector(".settings") || document.body.textContent.includes("偏好"),
    );
    const first = await application.evaluate(({ BrowserWindow }) =>
      BrowserWindow.getAllWindows().map(w => ({
        id: w.id,
        parent: w.getParentWindow()?.id,
        bounds: w.getBounds(),
        preferences: w.webContents.getLastWebPreferences(),
      })),
    );
    const child = first.find(w => w.parent);
    assert.ok(child);
    for (const w of first) {
      assert.equal(w.preferences.nodeIntegration, false);
      assert.equal(w.preferences.contextIsolation, true);
      assert.equal(w.preferences.sandbox, true);
    }
    await settings.evaluate(() => {
      window.retainedDraft = "retained";
    });
    const visible = () =>
      application.evaluate(
        ({ BrowserWindow }, id) => BrowserWindow.fromId(id).isVisible(),
        child.id,
      );
    const bounds = () =>
      application.evaluate(
        ({ BrowserWindow }, id) => BrowserWindow.fromId(id).getBounds(),
        child.id,
      );
    // The page can load before its window is shown, and X11 reports a
    // window's place a moment after it appears: wait for two equal readings.
    await expect.poll(visible).toBe(true);
    let placed;
    await expect
      .poll(async () => {
        const previous = placed;
        placed = await bounds();
        return JSON.stringify(placed) === JSON.stringify(previous);
      })
      .toBe(true);
    await settings.keyboard.press(`${modifier}+w`);
    await expect.poll(visible).toBe(false);
    await page.evaluate(() => window.asterionDesktop.openSettings("preferences"));
    assert.equal(application.windows().length, 2);
    assert.equal(await settings.evaluate(() => window.retainedDraft), "retained");
    await expect.poll(bounds).toEqual(placed);
    assert.equal(await page.getByLabel("品种代码", { exact: true }).inputValue(), "CU");
    // Real Chromium zoom exercises the installed host's layout, not a CSS transform.
    for (const window of [page, settings]) {
      const contents = await application.browserWindow(window);
      await contents.evaluate(win => win.webContents.setZoomFactor(1.25));
      await window.evaluate(
        () => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))),
      );
      const overflow = await window.evaluate(() => {
        const main = document.querySelector(".settings-content, .terminal-business");
        return {
          root: document.documentElement.scrollWidth > innerWidth,
          content: main ? main.scrollWidth > main.clientWidth + 1 : true,
        };
      });
      assert.deepEqual(
        overflow,
        { root: false, content: false },
        "Desktop content must fit at 125% zoom",
      );
      await capture(window, window === settings ? "native-settings-125" : "native-workbench-125");
      await contents.evaluate(win => win.webContents.setZoomFactor(1));
    }
    await settings.getByRole("button", { name: "连接与部署", exact: true }).click();
    const locations = settings.getByRole("list", { name: "当前运行位置", exact: true });
    await expect(locations).toBeVisible();
    for (const service of ["历史数据服务", "任务服务"])
      await expect(locations.getByRole("listitem").filter({ hasText: service })).toContainText(
        "本机",
      );
    await capture(settings, "native-deployment-overview");
    const deploymentWindow = await application.browserWindow(settings);
    await deploymentWindow.evaluate(win => win.webContents.setZoomFactor(1.25));
    await settings.getByRole("button", { name: "添加远程机器", exact: true }).click();
    await expect(settings.getByRole("region", { name: "添加远程机器", exact: true })).toContainText(
      "Linux x86_64",
    );
    assert.equal(
      await settings.evaluate(
        () =>
          document.documentElement.scrollWidth <= innerWidth &&
          document.querySelector(".settings-content").scrollWidth <=
            document.querySelector(".settings-content").clientWidth + 1,
      ),
      true,
    );
    await capture(settings, "native-deployment-wizard-125");
    await settings
      .getByRole("region", { name: "添加远程机器", exact: true })
      .getByRole("button", { name: "取消", exact: true })
      .click();
    await deploymentWindow.evaluate(win => win.webContents.setZoomFactor(1));
    await page.evaluate(() => {
      // The public settings helper selects reused windows through this storage event.
      localStorage.setItem(
        "asterion.settings.category",
        JSON.stringify({ page: "sources", nonce: Date.now() }),
      );
      return window.asterionDesktop.openSettings("sources");
    });
    await expect(settings.getByRole("heading", { name: "数据源", exact: true })).toBeVisible();
    await settings.getByRole("button", { name: "偏好设置", exact: true }).click();
    await capture(settings, "native-settings");
    await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
    await expect(page.getByLabel("Product Code", { exact: true })).toHaveValue("CU");
    await settings.getByLabel("Language", { exact: true }).selectOption("zh-CN");
    await expect(page.getByLabel("品种代码", { exact: true })).toHaveValue("CU");
    // Exercise renderer -> preload -> validated IPC -> native dialog adapter.
    await application.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async (_owner, opts) => ({
        canceled: false,
        filePaths: opts.properties.includes("openDirectory")
          ? ["/test/directory"]
          : ["/test/file.csv"],
      });
      dialog.showSaveDialog = async () => ({ canceled: true });
    });
    assert.equal(
      await settings.evaluate(() => window.asterionDesktop.open({ directory: true })),
      "/test/directory",
    );
    assert.equal(
      await settings.evaluate(() => window.asterionDesktop.save({ title: "Export" })),
      null,
    );
    assert.equal(await page.evaluate(() => window.open("https://example.com") === null), true);
    // The workspace in use and the managed Agent survive a whole desktop restart.
    await settings.evaluate(() => window.asterionDesktop.close());

    await page.reload();
    await enterWorkbench(page);
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    const ownedPid = Number(await fs.readFile(path.join(temp, "node", "agent.pid"), "utf8"));
    await closeDesktop(application);
    application = undefined;
    process.kill(ownedPid, 0); // Closing the desktop must not stop its managed Agent.
    application = await launch();
    const restored = await application.firstWindow();
    await enterWorkbench(restored);
    await expect(
      restored.getByRole("navigation", { name: "业务工作区" }).getByRole("button"),
    ).toHaveCount(6);
    await expect(
      restored.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }),
    ).toHaveAttribute("aria-current", "page", { timeout: 60000 });
    await restored
      .locator(".workspace-tabs")
      .getByRole("button", { name: "自选", exact: true })
      .click();
    await expect(restored.locator(".watchlist-workspace")).toBeVisible();
    await capture(restored, "native-watchlist");
    console.log(
      "Electron: direct startup without Cloud, no authentication IPC, startup, real C++ bridge, renderer isolation, settings ownership/reuse/bounds, cross-window locale, retained draft, dialog IPC, shortcuts, Agent independence and workspace restore after desktop restart passed",
    );
  } finally {
    try {
      if (application) await closeDesktop(application);
    } finally {
      const pidFile = path.join(temp, "node", "agent.pid");
      try {
        process.kill(Number(await fs.readFile(pidFile, "utf8")), "SIGTERM");
        await new Promise(resolve => setTimeout(resolve, 2000));
      } catch (error) {
        if (!["ENOENT", "ESRCH"].includes(error.code)) throw error;
      }
      await fs.rm(temp, { recursive: true, force: true });
    }
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
