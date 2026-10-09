const { spawn } = require("node:child_process");
delete process.env.ELECTRON_RUN_AS_NODE;
const net = require("node:net");
const path = require("node:path");
const root = path.resolve(__dirname, "../..");
const vite = path.join(root, "node_modules/vite/bin/vite.js");
const port = Number(process.env.ASTERION_DESKTOP_PORT ?? 1422);
if (!Number.isInteger(port) || port < 1024 || port > 65535)
  throw new Error("Invalid desktop development port");
let renderer,
  desktop,
  stopping = false,
  finishing = false;
function stop(code = 0) {
  process.exitCode = code;
  if (stopping) return;
  stopping = true;
  if (desktop && desktop.exitCode === null && desktop.signalCode === null) {
    // Keep Vite available while Electron waits for native work and stops services.
    if (desktop.connected) desktop.send({ type: "asterion:development-quit" });
    else desktop.kill("SIGTERM");
  } else if (!desktop) renderer?.kill();
}
async function finish(code) {
  if (finishing) return;
  finishing = true;
  stopping = true;
  // Handles renderer crashes and forced Electron termination as well as normal exit.
  const cleanup = spawn(process.execPath, [path.join(__dirname, "stop-development.cjs")], {
    cwd: root,
    env: process.env,
    stdio: "inherit",
    detached: true,
  });
  const result = await new Promise(resolve => {
    cleanup.once("error", () => resolve(1));
    cleanup.once("exit", value => resolve(value ?? 1));
  });
  process.exitCode = result || code || 0;
  renderer?.kill();
}
process.on("SIGINT", () => stop());
process.on("SIGTERM", () => stop());
const probe = net.createServer();
probe.once("error", () => {
  console.error("Desktop development port is occupied; close the other development entry point.");
  process.exitCode = 1;
});
probe.listen(port, "127.0.0.1", () =>
  probe.close(() => {
    if (stopping) return;
    renderer = spawn(process.execPath, [vite, "--config", "apps/clients/terminal/vite.config.ts"], {
      cwd: root,
      env: { ...process.env, ASTERION_DESKTOP_DEV: "1", ASTERION_DEV_PORT: String(port) },
      stdio: "inherit",
      detached: true,
    });
    renderer.once("error", error => {
      console.error(error.message);
      stop(1);
    });
    renderer.once("exit", code => {
      if (!stopping) stop(code ?? 1);
    });
    const deadline = Date.now() + 30000;
    (async () => {
      while (!stopping) {
        try {
          if ((await fetch(`http://127.0.0.1:${port}/index.html`)).ok) break;
        } catch {}
        if (Date.now() > deadline) throw new Error("Desktop frontend did not start");
        await new Promise(resolve => setTimeout(resolve, 100));
      }
      if (stopping) return;
      desktop = spawn(require("./electron-path.cjs")(), ["apps/clients/terminal/electron"], {
        cwd: root,
        env: { ...process.env, ASTERION_DEV_URL: `http://127.0.0.1:${port}` },
        stdio: ["inherit", "inherit", "inherit", "ipc"],
        detached: true,
      });
      process.send?.({ type: "development-desktop", pid: desktop.pid });
      desktop.once("error", error => {
        console.error(error.message);
        void finish(1);
      });
      desktop.once("exit", code => void finish(code ?? 1));
    })().catch(error => {
      console.error(error.message);
      stop(1);
    });
  }),
);
