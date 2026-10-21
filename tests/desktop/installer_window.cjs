// Read-only installer smoke; both UI and native inspection use disposable user paths.
// This never installs, starts or removes a system service.
const { _electron: electron } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const root = path.resolve(__dirname, "../..");
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-installer-window-"));
  const packaged = process.argv[2];
  const env = Object.fromEntries(
    Object.entries(process.env).filter(
      ([key]) => !key.startsWith("ASTERION_") && key !== "ELECTRON_RUN_AS_NODE",
    ),
  );
  Object.assign(env, {
    HOME: temporary,
    XDG_CONFIG_HOME: path.join(temporary, "config"),
    XDG_DATA_HOME: path.join(temporary, "data"),
    XDG_CACHE_HOME: path.join(temporary, "cache"),
  });
  let application;
  try {
    application = await electron.launch({
      executablePath: packaged
        ? path.resolve(packaged)
        : require("../../scripts/desktop/electron-path.cjs")(),
      args: [
        ...(packaged ? [] : [path.join(root, "apps/clients/installer/electron")]),
        ...(process.platform === "linux" ? ["--ozone-platform=x11"] : []),
      ],
      env,
      chromiumSandbox: true,
      timeout: 30000,
    });
    const page = await application.firstWindow();
    await page.waitForFunction(() =>
      document.querySelector("dd")?.textContent.includes("asterion-terminal"),
    );
    const state = await page.evaluate(() => window.asterionInstaller.inspect());
    assert.equal(state.installed, false);
    assert.equal(state.source_available, !!packaged);
    assert.equal(state.deployment_action, packaged ? "install" : "");
    assert.equal(state.preserve_data, true);
    assert.equal(state.conflict, "");
    assert.deepEqual(state.busy, []);
    assert.ok(state.directory.startsWith(temporary));
    assert.ok(state.node.startsWith(temporary));
    if (packaged) assert.ok(state.candidate_version);
    assert.equal(await page.evaluate(() => typeof window.require), "undefined");
    assert.equal(await page.evaluate(() => typeof window.process), "undefined");
    assert.equal(await page.locator("nav button").count(), 1);
    assert.equal(await page.locator("nav button").isDisabled(), !packaged);
    await page.evaluate(
      () => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))),
    );
    const window = await application.browserWindow(page);
    const pixels = await window.evaluate(async win =>
      (await win.webContents.capturePage()).toPNG().toString("base64"),
    );
    await fs.mkdir(path.join(root, "build/reviews"), { recursive: true });
    await fs.writeFile(
      path.join(root, `build/reviews/installer-window${packaged ? "-packaged" : ""}.png`),
      Buffer.from(pixels, "base64"),
    );
    if (packaged) {
      // A disposable registration fixture represents an installation whose files
      // need repair. Exercise real inspection/IPC, without deploying any services.
      await fs.mkdir(state.node, { recursive: true });
      await fs.writeFile(
        path.join(state.node, "installation.json"),
        JSON.stringify({
          product: "asterion-terminal",
          version: state.candidate_version,
          directory: state.directory,
          pending: "",
        }),
      );
      await page.getByRole("button", { name: "重新检查" }).click();
      await page.waitForFunction(
        () => document.querySelector("h1")?.textContent === "维护 Terminal",
      );
      const registered = await page.evaluate(() => window.asterionInstaller.inspect());
      assert.equal(registered.deployment_action, "repair");
      assert.equal(await page.locator("nav button").count(), 2);
      assert.equal(
        await page.locator("nav").getByRole("button", { name: /修复/ }).isDisabled(),
        false,
      );
      assert.equal(
        await page.locator("nav").getByRole("button", { name: /卸载/ }).isDisabled(),
        false,
      );
      const maintenance = await window.evaluate(async win =>
        (await win.webContents.capturePage()).toPNG().toString("base64"),
      );
      await fs.writeFile(
        path.join(root, "build/reviews/installer-window-maintenance.png"),
        Buffer.from(maintenance, "base64"),
      );
      await page.locator("nav").getByRole("button", { name: /卸载/ }).click();
      const retention = page.getByRole("checkbox", { name: "保留配置与业务数据" });
      assert.equal(await retention.isChecked(), true);
      await retention.uncheck();
      assert.equal(await retention.isChecked(), false);
      assert.ok((await page.getByRole("alert").textContent()).includes(state.node));
      assert.equal(await page.getByRole("button", { name: "确认执行 · 卸载" }).isDisabled(), true);
      // Only inspect the choice; never execute maintenance against the OS here.
      await page.evaluate(
        () => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))),
      );
      const removal = await window.evaluate(async win =>
        (await win.webContents.capturePage()).toPNG().toString("base64"),
      );
      await fs.writeFile(
        path.join(root, "build/reviews/installer-window-uninstall.png"),
        Buffer.from(removal, "base64"),
      );
    }
    console.log(
      `Installer window (${packaged ? "packaged" : "development"}), isolated native inspection and renderer boundary passed`,
    );
  } finally {
    if (application) await application.close();
    await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
