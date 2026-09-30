const { spawn } = require("node:child_process");
// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const net = require("node:net");
const path = require("node:path");
const root = path.resolve(__dirname, "..");
const vite = path.join(root, "node_modules/vite/bin/vite.js");
const port = Number(process.env.ASTERION_DESKTOP_PORT ?? 1422);
if (!Number.isInteger(port) || port < 1024 || port > 65535)
  throw new Error("Invalid desktop development port");
let renderer,
  desktop,
  stopping = false;
function stop(code = 0) {
  if (stopping) return;
  stopping = true;
  renderer?.kill();
  desktop?.kill();
  process.exitCode = code;
}
process.on("SIGINT", () => stop());
process.on("SIGTERM", () => stop());
const probe = net.createServer();
probe.once("error", () => {
  console.error(
    "Desktop development port is occupied; choose ASTERION_DESKTOP_PORT or close that development server.",
  );
  process.exitCode = 1;
});
probe.listen(port, "127.0.0.1", () =>
  probe.close(() => {
    renderer = spawn(process.execPath, [vite, "--config", "apps/clients/terminal/vite.config.ts"], {
      cwd: root,
      env: { ...process.env, ASTERION_DESKTOP_DEV: "1", ASTERION_DEV_PORT: String(port) },
      stdio: "inherit",
    });
    renderer.once("error", error => {
      console.error(error.message);
      stop(1);
    });
    renderer.once("exit", code => stop(code ?? 1));
    const deadline = Date.now() + 30000;
    (async () => {
      while (!stopping) {
        try {
          const response = await fetch(`http://127.0.0.1:${port}/index.html`);
          if (response.ok) break;
        } catch {}
        if (Date.now() > deadline) throw new Error("Desktop frontend did not start");
        await new Promise(resolve => setTimeout(resolve, 100));
      }
      if (stopping) return;
      desktop = spawn(require("electron"), ["apps/clients/terminal/electron"], {
        cwd: root,
        env: { ...process.env, ASTERION_DEV_URL: `http://127.0.0.1:${port}` },
        stdio: "inherit",
      });
      desktop.once("error", error => {
        console.error(error.message);
        stop(1);
      });
      desktop.once("exit", code => stop(code ?? 1));
    })().catch(error => {
      console.error(error.message);
      stop(1);
    });
  }),
);
