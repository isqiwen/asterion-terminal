// Opt-in acceptance of the real, persistent development profile. Does not log in
// to providers or create trades. Verifies automatic cleanup of the local services.
delete process.env.ELECTRON_RUN_AS_NODE;
const assert = require("node:assert/strict");
const { enterWorkbench } = require("./electron_startup.cjs");
const fs = require("node:fs/promises");
const path = require("node:path");
const crypto = require("node:crypto");
const { spawn } = require("node:child_process");
const local = require("./electron_local.cjs");
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
  assert.ok(!process.env.ASTERION_ROOT, "Must exercise the default development directory");
  const { production, development } = local;
  const definition = local.definition("me.asterion.node-agent.dev");
  const protectedFiles = [
    local.definition("me.asterion.node-agent"),
    path.join(production, "agent/bin/asterion_node_agent"),
    path.join(production, "agent/data/ipc-id"),
    path.join(production, "agent/data/agent.pid"),
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
        cwd: path.resolve(__dirname, "../.."),
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
      local.profile("me.asterion.terminal.dev"),
    );
    assert.equal(await application.evaluate(() => process.env.ASTERION_ENVIRONMENT), "development");
    await expect(page.locator(".environment-label")).toBeVisible();
    await enterWorkbench(page);
    await expect(page.locator(".workspace-tabs button")).toHaveCount(7, { timeout: 60000 });
    const snapshot = () =>
      page.evaluate(async () => {
        const response = JSON.parse(
          await window.asterionDesktop.request(
            JSON.stringify({ version: 1, method: "runtime.snapshot", params: {} }),
          ),
        );
        if (response.error) throw new Error(response.error.message);
        return response.result;
      });
    await expect
      .poll(async () => (await snapshot()).market?.transport_online, { timeout: 30000 })
      .toBe(true);
    await expect
      .poll(
        async () =>
          (await snapshot()).nodes
            .find(node => node.id === "local")
            ?.health?.services.find(service => service.kind === "lab")?.health,
        { timeout: 30000 },
      )
      .toBe("ready");
    assert.ok((await fs.readFile(definition, "utf8")).includes(development));
    const pid = Number(await fs.readFile(path.join(development, "agent/data/agent.pid"), "utf8"));
    assert.equal(await local.servicePid("me.asterion.node-agent.dev"), pid);
    const productionId = await fingerprint(path.join(production, "agent/data/ipc-id"));
    assert.notEqual(await fingerprint(path.join(development, "agent/data/ipc-id")), productionId);
    const trading = page
      .getByRole("navigation", { name: /^(业务工作区|Workspaces)$/ })
      .getByRole("button", { name: /^(实盘|Live)$/ });
    await trading.click();
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
    await assert.rejects(fs.stat(definition), { code: "ENOENT" });
    application = await launch();
    page = await application.firstWindow();
    await enterWorkbench(page);
    await expect(
      page
        .getByRole("navigation", { name: /^(业务工作区|Workspaces)$/ })
        .getByRole("button", { name: /^(实盘|Live)$/ }),
    ).toHaveAttribute("aria-current", "page", { timeout: 60000 });
    assert.notEqual(
      Number(await fs.readFile(path.join(development, "agent/data/agent.pid"), "utf8")),
      pid,
    );
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
          profile: local.profile("me.asterion.terminal.dev"),
          checks: [
            "default profile",
            "independent user service",
            "market transport and Python lab ready",
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
