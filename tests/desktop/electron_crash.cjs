delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const { testNode } = require("./electron_startup.cjs");

(async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-crash-"));
  const logs = path.join(root, "node/terminal/logs");
  let application;
  const launch = () =>
    electron.launch({
      executablePath: require("../../scripts/desktop/electron-path.cjs")(),
      chromiumSandbox: true,
      args: [path.resolve("apps/clients/terminal/electron"), `--user-data-dir=${root}/ui`],
      env: { ...process.env, ASTERION_ROOT: `${root}/node`, ...testNode },
    });
  const text = async () => {
    const files = (await fs.readdir(logs)).filter(
      file => file.startsWith("electron-") && file.endsWith(".log"),
    );
    return (await Promise.all(files.map(file => fs.readFile(path.join(logs, file), "utf8")))).join(
      "\n",
    );
  };
  try {
    application = await launch();
    const page = await application.firstWindow();
    await expect(page.locator(".terminal-shell")).toBeVisible();
    assert.equal(
      await application.evaluate(({ crashReporter }) => crashReporter.getUploadToServer()),
      false,
    );
    await page.evaluate(() => {
      setTimeout(function rendererFailure() {
        throw new Error("fixture renderer failure");
      }, 0);
      void Promise.reject(new Error("fixture renderer rejection"));
    });
    await expect.poll(text).toContain("rendererFailure");
    await expect.poll(text).toContain("fixture renderer rejection");
    const crashed = application.waitForEvent("close");
    await application.evaluate(({ BrowserWindow }) => {
      setTimeout(() => BrowserWindow.getAllWindows()[0].webContents.forcefullyCrashRenderer(), 100);
    });
    await crashed;
    application = null;
    await expect.poll(text).toContain("render-process-gone");
    await expect
      .poll(
        async () =>
          (await fs.readdir(path.join(logs, "crashes"), { recursive: true })).filter(file =>
            file.endsWith(".dmp"),
          ).length,
        { timeout: 15000 },
      )
      .toBeGreaterThan(0);
    application = await launch();
    const stopped = application.waitForEvent("close");
    await application.evaluate(() => {
      setTimeout(function mainFailure() {
        throw new Error("fixture main failure");
      }, 100);
    });
    await stopped;
    application = null;
    assert.match(await text(), /uncaughtException[\s\S]*fixture main failure[\s\S]*mainFailure/);
    console.log(
      "Desktop crashes: renderer and main stacks, local minidump, uploads disabled passed",
    );
  } finally {
    if (application) await application.close();
    await fs.rm(root, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
