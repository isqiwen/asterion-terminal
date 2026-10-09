// Opt-in lifecycle acceptance of the real local development environment.
delete process.env.ELECTRON_RUN_AS_NODE;
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const { createHash } = require("node:crypto");
const local = require("./electron_local.cjs");
const { spawn } = require("node:child_process");
const execFile = require("node:util").promisify(require("node:child_process").execFile);
const root = path.resolve(__dirname, "../..");
const nodeRoot = local.development;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
function running(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    if (error.code === "ESRCH") return false;
    throw error;
  }
}
async function status() {
  const identity = (await fs.readFile(path.join(nodeRoot, "ipc-id"), "utf8")).trim();
  const result = await execFile(
    path.join(root, "build/Debug/asterion_test_node_service_control"),
    [
      "--operation",
      "status",
      "--executable",
      path.join(nodeRoot, "bin/asterion-node-agent"),
      "--root",
      nodeRoot,
      "--endpoint",
      `/tmp/ast-node-${identity}/node.sock`,
      "--name",
      "me.asterion.acceptance.inspect",
    ],
    { timeout: 5000 },
  );
  return JSON.parse(result.stdout).health;
}
async function verifyPrograms(services) {
  const native = path.join(root, "build/electron-resources/native");
  const digest = async file =>
    createHash("sha256")
      .update(await fs.readFile(file))
      .digest("hex");
  assert.equal(
    await digest(path.join(nodeRoot, "bin/asterion-node-agent")),
    await digest(path.join(native, "asterion-node-agent")),
  );
  const programs = {
    market: {
      artifact: "asterion-market-data",
      provider_artifact: "ctp-md" + local.library,
      catalog_artifact: "ctp-trader" + local.library,
    },
    data: { artifact: "asterion-data-service" },
    task: {
      artifact: "asterion-task-service",
      worker_artifact: "asterion-backtest",
      factor_artifact: "asterion-factor",
      data_artifact: "asterion-data-pipeline",
    },
    live: { artifact: "asterion-trading", catalog_artifact: "ctp-trader" + local.library },
  };
  for (const service of services) {
    const config = JSON.parse(
      await fs.readFile(path.join(nodeRoot, "services", service.id, "service.json"), "utf8"),
    );
    assert.ok(programs[service.kind], `Unknown service kind ${service.kind}`);
    for (const [field, file] of Object.entries(programs[service.kind])) {
      assert.equal(config[field], await digest(path.join(native, file)), `${service.id}.${field}`);
      assert.equal(
        await digest(path.join(nodeRoot, "artifacts", `${config[field]}.bin`)),
        config[field],
      );
    }
  }
}
async function waitFor(check, timeout = 60000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const result = await check();
    if (result) return result;
    await delay(250);
  }
  throw new Error("Development lifecycle acceptance timed out");
}
(async () => {
  assert.ok(process.argv.includes("--managed-development"));
  assert.ok(!process.env.ASTERION_NODE_DIRECTORY);
  const scenarios = process.argv.includes("--full-entry")
    ? ["pnpm"]
    : ["interrupt", "crash", "browser"];
  for (const scenario of scenarios) {
    const browser = scenario === "browser";
    const child = spawn(
      scenario === "pnpm" ? "pnpm" : process.execPath,
      scenario === "pnpm"
        ? ["desktop"]
        : browser
          ? ["node_modules/vite/bin/vite.js", "--config", "apps/clients/terminal/vite.config.ts"]
          : ["scripts/desktop/electron-dev.cjs"],
      { cwd: root, env: process.env, detached: true, stdio: ["ignore", "pipe", "pipe", "ipc"] },
    );
    let output = "",
      electronPid,
      exited = false;
    child.stdout.on("data", chunk => {
      output += chunk;
    });
    child.stderr.on("data", chunk => {
      output += chunk;
    });
    child.on("message", message => {
      if (message.type === "development-desktop") electronPid = message.pid;
    });
    child.on("exit", () => {
      exited = true;
    });
    try {
      if (browser) {
        await waitFor(async () => {
          try {
            return (await fetch("http://127.0.0.1:1423")).ok;
          } catch {
            return false;
          }
        });
        const response = await fetch("http://127.0.0.1:1423/__asterion/api", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ version: 1, method: "market.local", params: {} }),
        });
        assert.ok((await response.json()).result);
      }
      const state = await waitFor(
        async () => {
          if (exited) throw new Error(`Development entry exited before startup:\n${output}`);
          try {
            const value = await status();
            return value?.services?.some(s => s.state === "running") &&
              value.services.filter(s => s.desired_running).every(s => s.state === "running")
              ? value
              : false;
          } catch {
            return false;
          }
        },
        scenario === "pnpm" ? 180000 : 60000,
      );
      if (scenario !== "browser") await verifyPrograms(state.services);
      const pids = [state.pid, ...state.services.map(s => s.pid).filter(Boolean)];
      if (scenario === "crash") {
        assert.ok(electronPid);
        process.kill(electronPid, "SIGKILL");
      } else process.kill(-child.pid, "SIGINT");
      await waitFor(async () => exited && pids.every(pid => !running(pid)));
      await waitFor(async () => {
        try {
          await fs.stat(local.definition("me.asterion.node-agent.dev"));
          return false;
        } catch (error) {
          if (error.code === "ENOENT") return true;
          throw error;
        }
      });
      const port = browser ? 1423 : 1422;
      await waitFor(async () => {
        try {
          await fetch(`http://127.0.0.1:${port}`);
          return false;
        } catch {
          return true;
        }
      });
      console.log(`${scenario}: Agent, services, developer entry and frontend stopped`);
    } catch (error) {
      console.error(output.slice(-10000));
      throw error;
    } finally {
      if (!exited) child.kill("SIGTERM");
    }
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
