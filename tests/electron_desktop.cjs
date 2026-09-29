const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
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
    await page.screenshot({ path: path.join(process.env.ASTERION_UI_SCREENSHOTS, name + ".png") });
  };
  const launch = () =>
    electron.launch({
      chromiumSandbox: true,
      executablePath: process.env.ASTERION_TEST_ELECTRON || require("electron"),
      args: [
        ...(process.env.ASTERION_TEST_ELECTRON
          ? []
          : [path.resolve("apps/clients/terminal/electron")]),
        `--user-data-dir=${temp}/ui`,
      ],
      env: {
        ...process.env,
        ASTERION_NODE_DIRECTORY: `${temp}/node`,
        ASTERION_TEST_NODE_ISOLATED: "1",
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
    if (process.platform === "darwin") {
      await expect(page.locator("html")).toHaveAttribute("data-integrated-titlebar", "macos");
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
    }
    const reply = await page.evaluate(async () =>
      JSON.parse(
        await window.asterionDesktop.request(
          JSON.stringify({ version: 1, method: "runtime.snapshot", params: {} }),
        ),
      ),
    );
    assert.equal(reply.result.protocol, 1);
    await expect(page.getByRole("button", { name: "开始设置", exact: true })).toBeVisible();
    await capture(page, "native-startup");
    await page.getByRole("button", { name: "开始设置", exact: true }).click();
    await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 60000 });
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
    await columnButton.click();
    const columnDialog = page.getByRole("dialog", { name: "行情表头设置" });
    await expect(columnDialog).toBeVisible();
    await columnDialog.getByRole("checkbox", { name: "持仓量", exact: true }).uncheck();
    await columnDialog.getByRole("button", { name: "应用列设置", exact: true }).click();
    await expect(columnButton).toBeFocused();
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await page
      .getByRole("navigation", { name: "业务工作区" })
      .getByRole("button", { name: "市场", exact: true })
      .click();
    await columnButton.click();
    await expect(
      columnDialog.getByRole("checkbox", { name: "持仓量", exact: true }),
    ).not.toBeChecked();
    await capture(page, "native-quote-columns");
    await page.keyboard.press("Escape");
    await expect(columnDialog).toHaveCount(0);
    await expect(columnButton).toBeFocused();

    await page.getByRole("button", { name: "数据", exact: true }).click();
    await page.getByRole("button", { name: "数据存档与结算表", exact: true }).click();
    await page.getByLabel("结算表 CSV 路径").fill("electron-retained-draft.csv");
    await page.getByRole("button", { name: "历史数据", exact: true }).click();
    const minuteDownloads = page.getByRole("region", { name: "历史数据", exact: true });
    await minuteDownloads.getByLabel("品种代码", { exact: true }).fill("CU");
    await minuteDownloads
      .getByLabel("Tushare Token", { exact: true })
      .fill("electron-fixture-secret");

    await page.getByRole("button", { name: "研究", exact: true }).click();
    await page.getByLabel("初始资金", { exact: true }).fill("25000");
    await page.getByRole("button", { name: "选择已发布成交数据", exact: true }).click();
    await expect(page.getByRole("region", { name: "数据发布", exact: true })).toBeVisible();
    await expect(page.getByLabel("CSV 文件路径", { exact: true })).toHaveCount(0);
    await page.getByRole("button", { name: "历史数据", exact: true }).click();
    await expect(minuteDownloads.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
    await expect(minuteDownloads.getByLabel("品种代码", { exact: true })).toHaveValue("CU");

    await page.getByRole("button", { name: "研究", exact: true }).click();
    await expect(page.getByLabel("初始资金", { exact: true })).toHaveValue("25000");
    await page.getByRole("button", { name: "数据", exact: true }).click();

    await page.getByRole("button", { name: "数据存档与结算表", exact: true }).click();
    await expect(page.getByLabel("结算表 CSV 路径")).toHaveValue("electron-retained-draft.csv");

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
    await settings.keyboard.press(`${modifier}+w`);
    await expect
      .poll(() =>
        application.evaluate(
          ({ BrowserWindow }, id) => BrowserWindow.fromId(id).isVisible(),
          child.id,
        ),
      )
      .toBe(false);
    await page.evaluate(() => window.asterionDesktop.openSettings("preferences"));
    assert.equal(application.windows().length, 2);
    assert.equal(await settings.evaluate(() => window.retainedDraft), "retained");
    const bounds = await application.evaluate(
      ({ BrowserWindow }, id) => BrowserWindow.fromId(id).getBounds(),
      child.id,
    );
    assert.deepEqual(bounds, child.bounds);
    assert.equal(
      await page.getByLabel("结算表 CSV 路径").inputValue(),
      "electron-retained-draft.csv",
    );
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
    await capture(settings, "native-settings");
    await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
    await expect(page.getByLabel("Settlement CSV Path")).toHaveValue("electron-retained-draft.csv");
    await settings.getByLabel("Language", { exact: true }).selectOption("zh-CN");
    await expect(page.getByLabel("结算表 CSV 路径")).toHaveValue("electron-retained-draft.csv");
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
    // Exercise actual UI controls and C++ ledger across a whole desktop restart.
    await settings.evaluate(() => window.asterionDesktop.close());
    await require("./electron_history.cjs")(page, temp, capture);
    const csv = path.join(temp, "ticks.csv"),
      account = path.join(temp, "paper-account");
    await fs.mkdir(account);
    await fs.writeFile(
      csv,
      "timestamp_ns,price,quantity\n1790384400000000000,100,1\n1790384401000000000,99,1\n1790384402000000000,110,1\n",
    );
    // Initialize a test-only existing dataset through the native bridge. There
    // is deliberately no historical file-import workflow in the product UI.
    const seeded = await page.evaluate(
      async file =>
        JSON.parse(
          await window.asterionDesktop.request(
            JSON.stringify({
              version: 1,
              method: "futures.inspect_csv",
              params: {
                path: file,
                venue: "SHFE",
                symbol: "rb2610",
                product: "rb",
                delivery_month: "2026-10",
                currency: "CNY",
                price_increment: "1",
                quantity_increment: "1",
                multiplier: "10",
              },
            }),
          ),
        ),
      csv,
    );
    assert.equal(seeded.error, undefined);
    await page.reload();
    await page.getByRole("button", { name: "交易", exact: true }).click();
    for (const [label, value] of [
      ["交易记录目录", account],
      ["初始模拟资金", "1000"],
      ["每手保证金", "100"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
      ["单笔数量上限", "1"],
      ["总持仓量上限", "1"],
      ["在途委托数上限", "1"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "创建模拟会话", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("1000 CNY");
    await page.getByRole("button", { name: "回放下一笔", exact: true }).click();
    await page.getByLabel("限价", { exact: true }).fill("100");
    await page.getByLabel("委托手数", { exact: true }).fill("2");
    await page.getByRole("button", { name: "提交模拟委托", exact: true }).click();
    await expect(page.getByRole("alert")).toBeVisible();
    await expect(page.getByTestId("paper-frozen")).toHaveText("0 CNY");
    await expect(page.getByRole("table", { name: "模拟委托" }).locator("tbody tr")).toHaveCount(0);
    await page.getByLabel("委托手数", { exact: true }).fill("1");
    await page.getByRole("button", { name: "提交模拟委托", exact: true }).click();
    await expect(page.getByTestId("paper-frozen")).toHaveText("102 CNY");
    await page.getByRole("button", { name: "回放下一笔", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("998 CNY");
    await expect(page.getByRole("table", { name: "模拟成交" }).locator("tbody tr")).toHaveCount(1);
    const ownedPid = Number(await fs.readFile(path.join(temp, "node", "agent.pid"), "utf8"));
    await closeDesktop(application);
    application = undefined;
    process.kill(ownedPid, 0); // Closing the desktop must not stop its managed Agent.
    application = await launch();
    const restored = await application.firstWindow();
    await restored.getByRole("button", { name: "交易", exact: true }).click({ timeout: 60000 });
    await restored.getByLabel("交易记录目录", { exact: true }).fill(account);
    await restored.getByRole("button", { name: "恢复会话", exact: true }).click();
    await expect(restored.getByTestId("paper-balance")).toHaveText("998 CNY");
    await expect(restored.getByTestId("paper-fees")).toHaveText("2 CNY");
    await expect(restored.getByRole("table", { name: "模拟持仓" })).toContainText("多头");
    await expect(restored.getByRole("table", { name: "模拟成交" }).locator("tbody tr")).toHaveCount(
      1,
    );
    await restored.getByLabel("买卖方向", { exact: true }).selectOption("sell");
    await restored.getByLabel("开平仓", { exact: true }).selectOption("close_today");
    await restored.getByLabel("限价", { exact: true }).fill("110");
    await restored.getByRole("button", { name: "提交模拟委托", exact: true }).click();
    await restored.getByRole("button", { name: "回放下一笔", exact: true }).click();
    await expect(restored.getByTestId("paper-balance")).toHaveText("1105 CNY");
    await expect(restored.getByTestId("paper-fees")).toHaveText("5 CNY");
    await expect(restored.getByRole("table", { name: "模拟持仓" }).locator("tbody tr")).toHaveCount(
      0,
    );
    await expect(restored.getByRole("table", { name: "模拟成交" }).locator("tbody tr")).toHaveCount(
      2,
    );
    await restored.locator(".terminal-business").evaluate(el => (el.scrollTop = 0));
    await capture(restored, "native-trading");
    await restored.getByRole("button", { name: "总览", exact: true }).click();
    await expect(restored.getByRole("heading", { name: "账户与持仓", exact: true })).toBeVisible();
    await capture(restored, "native-overview");
    console.log(
      "Electron: direct startup without Cloud, no authentication IPC, startup, real C++ bridge, renderer isolation, settings ownership/reuse/bounds, cross-window locale, retained draft, dialog IPC, shortcuts, Agent independence and paper ledger recovery after desktop restart passed",
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
