// Opt-in acceptance of the real, persistent development profile. Does not log in
// to providers or create trades. Verifies automatic cleanup of the local services.
delete process.env.ELECTRON_RUN_AS_NODE;
const assert = require("node:assert/strict");
const { enterWorkbench } = require("./electron_startup.cjs");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
const crypto = require("node:crypto");
const { spawn } = require("node:child_process");
const execFile = require("node:util").promisify(require("node:child_process").execFile);
const { _electron: electron, expect } = require("@playwright/test");

async function fingerprint(file) {
  try {
    return crypto
      .createHash("sha256")
      .update(await fs.readFile(file))
      .digest("hex");
  } catch (error) {
    if (error.code === "ENOENT") return null;
    throw error;
  }
}
async function main() {
  assert.ok(
    process.argv.includes("--managed-development"),
    "Requires explicit managed-development mode",
  );
  assert.equal(process.platform, "darwin");
  assert.ok(
    !process.env.ASTERION_NODE_DIRECTORY,
    "Must exercise the default development directory",
  );
  const support = path.join(os.homedir(), "Library/Application Support");
  const production = path.join(support, "Asterion/node");
  const development = path.join(support, "Asterion Development/node");
  const launchAgents = path.join(os.homedir(), "Library/LaunchAgents");
  const protectedFiles = [
    path.join(launchAgents, "me.asterion.node-agent.plist"),
    path.join(production, "bin/asterion-node-agent"),
    path.join(production, "ipc-id"),
    path.join(production, "agent.pid"),
  ];
  const before = await Promise.all(protectedFiles.map(fingerprint));
  const output = path.resolve("build/audit-review/environment");
  await fs.mkdir(output, { recursive: true });
  let application;
  let renderer;
  if (process.argv.includes("--vite")) {
    renderer = spawn(
      process.execPath,
      ["node_modules/vite/bin/vite.js", "--config", "apps/clients/terminal/vite.config.ts"],
      {
        cwd: path.resolve(__dirname, ".."),
        env: { ...process.env, ASTERION_DESKTOP_DEV: "1", ASTERION_DEV_PORT: "1422" },
        stdio: "ignore",
      },
    );
    const deadline = Date.now() + 30000;
    for (;;) {
      try {
        if ((await fetch("http://127.0.0.1:1422/index.html")).ok) break;
      } catch {}
      if (Date.now() > deadline) throw new Error("Vite startup timed out");
      await new Promise(resolve => setTimeout(resolve, 100));
    }
  }
  const launch = () =>
    electron.launch({
      args: [path.resolve("apps/clients/terminal/electron")],
      // Deliberately conflicting inherited value: the app must select its own profile.
      env: {
        ...process.env,
        ASTERION_ENVIRONMENT: "production",
        ...(renderer ? { ASTERION_DEV_URL: "http://127.0.0.1:1422" } : {}),
      },
    });
  try {
    application = await launch();
    let page = await application.firstWindow();
    await page.waitForFunction(() => !!window.asterionDesktop);
    assert.equal(
      await application.evaluate(({ app }) => app.getPath("userData")),
      path.join(support, "me.asterion.terminal.dev"),
    );
    assert.equal(await application.evaluate(() => process.env.ASTERION_ENVIRONMENT), "development");
    await expect(page.locator(".environment-label")).toBeVisible();
    await enterWorkbench(page);
    await expect(page.locator(".workspace-tabs button")).toHaveCount(6, { timeout: 60000 });
    await expect(page.locator(".service-status > button")).toHaveClass("good", { timeout: 30000 });
    const plist = await fs.readFile(
      path.join(launchAgents, "me.asterion.node-agent.dev.plist"),
      "utf8",
    );
    assert.ok(plist.includes(development));
    assert.ok(plist.includes("me.asterion.node-agent.dev"));
    const pid = Number(await fs.readFile(path.join(development, "agent.pid"), "utf8"));
    const { stdout } = await execFile("/bin/launchctl", [
      "print",
      `gui/${process.getuid()}/me.asterion.node-agent.dev`,
    ]);
    assert.ok(stdout.includes(`pid = ${pid}`));
    const productionId = await fingerprint(path.join(production, "ipc-id"));
    assert.notEqual(await fingerprint(path.join(development, "ipc-id")), productionId);
    await page.locator(".workspace-tabs button").last().click();
    await expect(
      page.getByRole("region", { name: /^(CTP 交易账户|CTP trading account)$/i }),
    ).toBeVisible();
    const window = await application.browserWindow(page);
    await fs.writeFile(
      path.join(output, "development.png"),
      Buffer.from(
        await window.evaluate(async win =>
          (await win.webContents.capturePage()).toPNG().toString("base64"),
        ),
        "base64",
      ),
    );
    const processes = await page.evaluate(async () => {
      const reply = JSON.parse(
        await window.asterionDesktop.request(
          JSON.stringify({ version: 1, method: "runtime.snapshot", params: {} }),
        ),
      );
      return reply.result.nodes
        .find(node => node.id === "local")
        .health.services.map(service => service.pid)
        .filter(Boolean);
    });
    // Exercise the actual window close control, not only Electron's app.quit().
    const desktopProcess = application.process();
    await window.evaluate(win => win.close());
    await expect.poll(() => desktopProcess.exitCode, { timeout: 60000 }).not.toBeNull();
    application = undefined;
    for (const processId of processes)
      assert.throws(() => process.kill(processId, 0), { code: "ESRCH" });
    assert.throws(() => process.kill(pid, 0), { code: "ESRCH" });
    await assert.rejects(fs.stat(path.join(launchAgents, "me.asterion.node-agent.dev.plist")), {
      code: "ENOENT",
    });
    application = await launch();
    page = await application.firstWindow();
    await enterWorkbench(page);
    await expect(page.locator(".workspace-tabs button").last()).toHaveAttribute(
      "aria-current",
      "page",
      { timeout: 60000 },
    );
    assert.notEqual(Number(await fs.readFile(path.join(development, "agent.pid"), "utf8")), pid);
    assert.deepEqual(
      await Promise.all(protectedFiles.map(fingerprint)),
      before,
      "Development startup altered the production Agent identity or program",
    );
    await fs.writeFile(
      path.join(output, "managed-development.json"),
      JSON.stringify(
        {
          status: "passed",
          development,
          profile: path.join(support, "me.asterion.terminal.dev"),
          checks: [
            "default profile",
            "independent launchd service",
            "different IPC identity",
            "production identity and program unchanged",
            "Agent and services stop on desktop exit",
            "workspace restored with a new Agent on restart",
          ],
        },
        null,
        2,
      ) + "\n",
    );
    console.log("Managed development environment isolation passed");
  } finally {
    if (application) await application.close();
    renderer?.kill();
  }
}
main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
