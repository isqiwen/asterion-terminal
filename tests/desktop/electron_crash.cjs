delete process.env.ELECTRON_RUN_AS_NODE;
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const { execFileSync } = require("node:child_process");
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
    await expect
      .poll(
        async () =>
          (await fs.readdir(path.join(logs, "crashes"))).filter(file =>
            file.endsWith(".report.log"),
          ).length,
        { timeout: 30000 },
      )
      .toBeGreaterThan(0);
    const reports = (await fs.readdir(path.join(logs, "crashes"))).filter(file =>
      file.endsWith(".report.log"),
    );
    const report = await fs.readFile(path.join(logs, "crashes", reports[0]), "utf8");
    assert.match(report, /Thread \d+/);
    // macOS captures a hung-renderer snapshot before terminating the process.
    assert.match(report, process.platform === "darwin" ? /^No crash$/m : /Crash reason:/);
    const stopped = application.waitForEvent("close");
    await application.evaluate(() => {
      setTimeout(function mainFailure() {
        throw new Error("fixture main failure");
      }, 100);
    });
    await stopped;
    application = null;
    assert.match(await text(), /uncaughtException[\s\S]*fixture main failure[\s\S]*mainFailure/);
    const build = path.resolve(process.env.ASTERION_CPP_BUILD || "build/Debug");
    const fixture = path.join(build, "asterion_native_crash_fixture.node");
    const sources = [fixture];
    if (process.platform === "darwin") {
      const bundle = path.join(root, "fixture.dSYM");
      execFileSync("dsymutil", [fixture, "-o", bundle]);
      sources.push(path.join(bundle, "Contents/Resources/DWARF", path.basename(fixture)));
    }
    const dumper = path.resolve(
      "build/crash-tools/dump_syms/2.3.9",
      process.platform === "darwin" ? "aarch64-apple-darwin" : "x86_64-unknown-linux-gnu",
      "dump_syms",
    );
    const symbols = execFileSync(dumper, ["--check-cfi", "--verbose", "off", ...sources], {
      encoding: "utf8",
      maxBuffer: 16 * 1024 * 1024,
    });
    const [, , , identifier, module] = symbols.split("\n", 1)[0].split(" ");
    const symbolDirectory = path.join(logs, "crashes/symbols", module, identifier);
    await fs.mkdir(symbolDirectory, { recursive: true });
    await fs.writeFile(path.join(symbolDirectory, module + ".sym"), symbols);
    application = await launch();
    const crashedProcess = application.process();
    const nativeCrash = new Promise(resolve => crashedProcess.once("exit", resolve));
    const nativeClosed = application.waitForEvent("close");
    await application.evaluate((_electron, fixture) => {
      const load = process
        .getBuiltinModule("module")
        .createRequire(process.cwd() + "/package.json");
      const crash = load(fixture).crash;
      setTimeout(crash, 100);
    }, fixture);
    await nativeCrash;
    // A fatal main-process crash can leave Chromium children holding Playwright pipes.
    try {
      process.kill(-crashedProcess.pid, "SIGKILL");
    } catch (error) {
      if (error.code !== "ESRCH") throw error;
    }
    await nativeClosed;
    application = null;
    application = await launch();
    await expect
      .poll(
        async () => {
          const reports = (await fs.readdir(path.join(logs, "crashes"))).filter(file =>
            file.endsWith(".report.log"),
          );
          return (
            await Promise.all(
              reports.map(file => fs.readFile(path.join(logs, "crashes", file), "utf8")),
            )
          ).join("\n");
        },
        { timeout: 30000 },
      )
      .toMatch(/Crash reason:[\s\S]*native_crash_site[\s\S]*crash_fixture.cpp/);
    console.log(
      "Desktop crashes: renderer and main stacks, automatically symbolicated native source stack, local minidump, uploads disabled passed",
    );
  } catch (error) {
    console.error(await text());
    for (const file of await fs.readdir(path.join(logs, "crashes"))) {
      if (file.endsWith(".errors.log"))
        console.error(await fs.readFile(path.join(logs, "crashes", file), "utf8"));
    }
    throw error;
  } finally {
    if (application) await application.close();
    await fs.rm(root, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
