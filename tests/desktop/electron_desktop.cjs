const { openView, openMarketSection } = require("./product-navigation.cjs");
// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const { enterWorkbench, testNode } = require("./electron_startup.cjs");
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
  // The test Market tests/support/e2e.py runs: this Terminal reads it with its reader identity.
  const identity = path.join(temp, "node/terminal/data/tls");
  await fs.mkdir(identity, { recursive: true });
  for (const extension of ["crt", "key"])
    await fs.copyFile(
      path.join(process.env.ASTERION_TEST_HISTORY_TLS, `reader.${extension}`),
      path.join(identity, `client.${extension}`),
    );
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
      recordVideo: process.env.ASTERION_UI_VIDEO_DIR
        ? {
            dir: path.join(process.env.ASTERION_UI_VIDEO_DIR, "electron_desktop"),
            size: { width: 1440, height: 900 },
          }
        : undefined,
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
        ASTERION_ROOT: `${temp}/node`,
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
    if (process.platform !== "darwin") {
      // Linux draws the window buttons over the bar's right end.
      const bar = await page.evaluate(() => {
        const free = navigator.windowControlsOverlay.getTitlebarAreaRect();
        return {
          overlay: navigator.windowControlsOverlay.visible,
          free: free.x + free.width,
          width: innerWidth,
        };
      });
      assert.ok(bar.overlay && bar.free < bar.width, JSON.stringify(bar));
    }
    // "Copy diagnostic" writes the system clipboard; reading it stays denied.
    await page.bringToFront();
    await page.evaluate(() => navigator.clipboard.writeText("asterion diagnostic"));
    assert.equal(
      await application.evaluate(({ clipboard }) => clipboard.readText()),
      "asterion diagnostic",
    );
    await assert.rejects(page.evaluate(() => navigator.clipboard.readText()));
    const reply = await page.evaluate(async () =>
      JSON.parse(
        await window.asterionDesktop.request(
          JSON.stringify({ version: 1, method: "runtime.snapshot", params: {} }),
        ),
      ),
    );
    assert.equal(reply.result.protocol, 1);
    // Startup runs by itself and opens the workbench once every step passes.
    // A package's first launch downloads the Python runtime (about 200 MB).
    await enterWorkbench(page, process.env.ASTERION_TEST_ELECTRON ? 600000 : undefined);
    await capture(page, "native-startup");
    const call = async (method, params = {}) => {
      const reply = JSON.parse(
        await page.evaluate(
          body => window.asterionDesktop.request(body),
          JSON.stringify({ version: 1, method, params }),
        ),
      );
      assert.equal(reply.error, undefined, JSON.stringify(reply.error));
      return reply.result;
    };
    await call("history.endpoint.save", {
      host: "127.0.0.1",
      port: Number(process.env.ASTERION_TEST_HISTORY_PORT),
      fingerprint: process.env.ASTERION_TEST_HISTORY_FINGERPRINT,
    });
    await call("quotes.endpoint.save", {
      host: "127.0.0.1",
      port: Number(process.env.ASTERION_TEST_QUOTES_PORT),
      fingerprint: process.env.ASTERION_TEST_QUOTES_FINGERPRINT,
    });
    await call("quotes.open");
    for (const [name, selector] of [
      ["contract", ".contract-workspace"],
      ["market", ".market-workspace"],
    ]) {
      await openView(page, name);
      await expect(page.locator(selector)).toBeVisible();
    }

    await call("quotes.watchlist", { instruments: [{ exchange: "SHFE", symbol: "rb2610" }] });
    await openMarketSection(page, "自选");
    const priceHeading = page
      .locator(".market-quote-table table th")
      .filter({ hasText: /^现价/ })
      .first();
    await priceHeading.getByRole("button").click();
    await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
    await openView(page, "contract");
    await openMarketSection(page, "自选");
    await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
    await openMarketSection(page, "期货全景");
    await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
    await openMarketSection(page, "自选");
    await expect(priceHeading).toHaveAttribute("aria-sort", "descending");
    await capture(page, "native-watchlist-sort");
    await openView(page, "market");

    const columnButton = page.getByRole("button", { name: "行情表头设置", exact: true });
    if (!(await page.getByRole("dialog", { name: "行情设置", exact: true }).count()))
      await page.getByRole("button", { name: "行情设置", exact: true }).click();
    await columnButton.click();
    const columnDialog = page.getByRole("dialog", { name: "行情表头设置" });
    await expect(columnDialog).toBeVisible();
    await columnDialog.getByRole("checkbox", { name: "持仓量", exact: true }).uncheck();
    await columnDialog.getByRole("button", { name: "应用列设置", exact: true }).click();
    await expect(columnButton).toBeFocused();
    await openView(page, "research");
    await openView(page, "market");
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

    // The research page reaches the lab service the Terminal starts.
    await openView(page, "research");
    await expect(page.getByRole("button", { name: /^MaCross ma_cross / })).toBeVisible();
    await capture(page, "native-research");
    // A draft of the market page, which must survive the settings window and a restart.
    await openView(page, "market");
    await page.getByRole("searchbox", { name: "搜索市场合约", exact: true }).fill("cu");

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
    assert.equal(
      await page.getByRole("searchbox", { name: "搜索市场合约", exact: true }).inputValue(),
      "cu",
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
    await page.evaluate(() => {
      // The public settings helper selects reused windows through this storage event.
      localStorage.setItem(
        "asterion.settings.category",
        JSON.stringify({ page: "market", nonce: Date.now() }),
      );
      return window.asterionDesktop.openSettings("market");
    });
    await expect(settings.getByRole("heading", { name: "行情服务", exact: true })).toBeVisible();
    await settings.getByRole("button", { name: "偏好设置", exact: true }).click();
    await capture(settings, "native-settings");
    await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
    await expect(
      page.getByRole("searchbox", { name: "Search market contracts", exact: true }),
    ).toHaveValue("cu");
    await settings.getByLabel("Language", { exact: true }).selectOption("zh-CN");
    await expect(page.getByRole("searchbox", { name: "搜索市场合约", exact: true })).toHaveValue(
      "cu",
    );
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
    // The workspace in use survives a whole desktop restart.
    await settings.evaluate(() => window.asterionDesktop.close());

    await page.reload();
    await enterWorkbench(page);
    await openView(page, "research");
    await closeDesktop(application);
    application = undefined;
    application = await launch();
    const restored = await application.firstWindow();
    await enterWorkbench(restored);
    await expect(
      restored.getByRole("navigation", { name: "业务工作区" }).getByRole("button"),
    ).toHaveCount(4);
    await expect(
      restored.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }),
    ).toHaveAttribute("aria-current", "page", { timeout: 60000 });
    await openMarketSection(restored, "自选");
    await expect(restored.locator(".market-workspace")).toBeVisible();
    await capture(restored, "native-watchlist");
    console.log(
      "Electron: direct startup without Cloud, no authentication IPC, startup, real C++ bridge, renderer isolation, settings ownership/reuse/bounds, cross-window locale, retained draft, dialog IPC, shortcuts and workspace restore after desktop restart passed",
    );
  } finally {
    try {
      if (application) await closeDesktop(application);
    } finally {
      // The services exit with the application that started them.
      await new Promise(resolve => setTimeout(resolve, 2000));
      await fs.rm(temp, { recursive: true, force: true });
    }
  }
})().catch(error => {
  console.error(error);
  // A child left running by the failure would otherwise keep Node waiting forever.
  process.exit(1);
});
