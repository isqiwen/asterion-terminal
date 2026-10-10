import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { createInterface } from "node:readline";
const root = fileURLToPath(new URL("../../..", import.meta.url));
// C++ build that serves the dev bridge.
const cppBuild = process.env.ASTERION_CPP_BUILD ?? resolve(root, "build/Debug");
const port = Number(process.env.ASTERION_DEV_PORT ?? 1423);
const allowedHosts = new Set([`127.0.0.1:${port}`, `localhost:${port}`]);

// Development-only transport to the same C++ API used by Electron. No fake backend.
function localCore(): Plugin {
  let closeCore: (() => Promise<void>) | undefined;
  return {
    async closeBundle() {
      await closeCore?.();
    },
    name: "asterion-local-core",
    configureServer(server) {
      // Vite handles SIGTERM, but a terminal process-group SIGINT can bypass
      // its close hooks. Wait for native cleanup before leaving this process.
      const interrupt = () => {
        void server.close().then(
          () => process.exit(),
          error => {
            server.config.logger.error(String(error));
            process.exit(1);
          },
        );
      };
      process.on("SIGINT", interrupt);
      const executable = "asterion_terminal_dev_bridge";
      // One C++ core process; isolated test runs replace it between spec files.
      const startCore = () => {
        const child = spawn(resolve(cppBuild, executable), [], {
          env: {
            ...process.env,
            ASTERION_ENVIRONMENT: "development",
            ASTERION_REMOTE_RESOURCES:
              process.env.ASTERION_REMOTE_RESOURCES ??
              resolve(root, "build/electron-resources/remote-linux"),
          },
          stdio: ["pipe", "pipe", "pipe"],
          detached: true,
        });
        const core = {
          child,
          pending: [] as { resolve: (response: string) => void; reject: (error: Error) => void }[],
          failed: null as Error | null,
        };
        const fail = (error: Error) => {
          core.failed = error;
          for (const item of core.pending.splice(0)) item.reject(error);
        };
        createInterface({ input: child.stdout }).on("line", line =>
          core.pending.shift()?.resolve(line),
        );
        child.on("error", fail);
        child.on("exit", () => fail(new Error("C++ 核心已退出，请重新启动开发服务")));
        child.stderr.on("data", data => server.config.logger.error(String(data)));
        return core;
      };
      let core = startCore();
      const originalNode = process.env.ASTERION_NODE_DIRECTORY;
      // Stops an isolated Agent by its recorded PID; its services follow it.
      const stopAgent = async (directory: string) => {
        const pidFile = resolve(directory, "agent.pid");
        if (!existsSync(pidFile)) return;
        const pid = Number(readFileSync(pidFile, "utf8"));
        const alive = () => {
          try {
            process.kill(pid, 0);
            return true;
          } catch {
            return false;
          }
        };
        if (!(pid > 0) || !alive()) return;
        process.kill(pid, "SIGTERM");
        for (let i = 0; i < 100 && alive(); ++i) await new Promise(r => setTimeout(r, 50));
        if (alive()) process.kill(pid, "SIGKILL");
      };
      const send = (request: string) =>
        new Promise<string>((resolve, reject) => {
          const current = core;
          if (current.failed) return reject(current.failed);
          current.pending.push({ resolve, reject });
          current.child.stdin.write(request + "\n", error => {
            if (error) {
              current.failed = error;
              for (const item of current.pending.splice(0)) item.reject(error);
            }
          });
        });
      const exited = (child: ReturnType<typeof spawn>) =>
        child.exitCode !== null || child.signalCode !== null
          ? Promise.resolve()
          : new Promise<void>(done => child.once("exit", () => done()));
      closeCore = async () => {
        process.off("SIGINT", interrupt);
        if (core.failed) {
          core.child.kill();
          throw core.failed;
        }
        if (process.env.ASTERION_NODE_DIRECTORY) {
          core.child.kill();
          return;
        }
        const response = JSON.parse(
          await send(
            JSON.stringify({
              version: 1,
              method: "development.shutdown",
              params: { recover: false },
            }),
          ),
        );
        if (response.error) {
          server.config.logger.error(
            `Development cleanup incomplete; services may remain: ${response.error.message}`,
          );
          core.child.kill();
          throw new Error(response.error.message);
        }
        core.child.kill();
      };
      // Isolated end-to-end runs only: a fresh core, Agent and node directory
      // per spec file, so one spec's services and data never affect the next.
      // The previous Agent is stopped by its recorded PID; its services follow
      // their supervisor.
      const isolated = process.env.ASTERION_TEST_NODE_ISOLATED === "1" && !!originalNode;
      const reset = async () => {
        core.child.kill();
        await exited(core.child);
        await stopAgent(process.env.ASTERION_NODE_DIRECTORY!);
        // A node directory holds a copy of every service program: the one
        // before goes, or a run of forty spec files fills the temporary
        // filesystem. Services that are still stopping may write once more.
        for (const entry of readdirSync(originalNode!))
          rmSync(resolve(originalNode!, entry), {
            recursive: true,
            force: true,
            maxRetries: 20,
            retryDelay: 100,
          });
        // Inside the wrapper's temporary root, which it cleans up at exit.
        process.env.ASTERION_NODE_DIRECTORY = mkdtempSync(resolve(originalNode!, "reset-"));
        core = startCore();
        return process.env.ASTERION_NODE_DIRECTORY;
      };
      const allowed = (req: { method?: string; headers: Record<string, unknown> }) => {
        // This middleware runs before Vite's own host check, so it enforces one
        // itself: a DNS-rebound page carries an attacker-chosen Host header.
        const host = String(req.headers.host ?? "");
        return (
          req.method === "POST" &&
          req.headers["content-type"] === "application/json" &&
          allowedHosts.has(host) &&
          (!req.headers.origin || req.headers.origin === `http://${host}`)
        );
      };
      if (isolated)
        server.middlewares.use("/__asterion/test/reset", (req, res) => {
          if (!allowed(req)) {
            res.statusCode = 403;
            res.end("Forbidden");
            return;
          }
          reset().then(
            directory => {
              res.setHeader("Content-Type", "application/json");
              res.end(JSON.stringify({ node_directory: directory }));
            },
            error => {
              res.statusCode = 500;
              res.end(String(error));
            },
          );
        });
      server.middlewares.use("/__asterion/api", (req, res) => {
        if (!allowed(req)) {
          res.statusCode = 403;
          res.end("Forbidden");
          return;
        }
        let body = "";
        req.on("data", chunk => {
          body += chunk.toString();
          if (body.length > 65536) req.destroy();
        });
        req.on("end", () => {
          if (core.failed) {
            res.statusCode = 503;
            res.end(core.failed.message);
            return;
          }
          // JSON framing prevents a body newline from becoming a second request.
          let request: string;
          try {
            request = JSON.stringify(JSON.parse(body));
          } catch {
            res.statusCode = 400;
            res.end("Invalid JSON");
            return;
          }
          send(request)
            .then(result => {
              res.setHeader("Content-Type", "application/json");
              res.end(result);
            })
            .catch(error => {
              res.statusCode = 503;
              res.end(String(error));
            });
        });
      });
    },
  };
}
export default defineConfig({
  root: resolve(root, "apps/clients/terminal"),
  plugins: [react(), ...(process.env.ASTERION_DESKTOP_DEV === "1" ? [] : [localCore()])],
  resolve: {
    alias: {
      "@asterion/client-ui": resolve(root, "packages/client-ui/src"),
      "@asterion/workbench": resolve(root, "apps/clients/terminal/src/host"),
      "@asterion/desktop-bridge": resolve(root, "apps/clients/terminal/src/bridge"),
      "@asterion/terminal": resolve(root, "apps/clients/terminal/src"),
    },
  },
  server: {
    host: "127.0.0.1",
    port,
    strictPort: true,
    fs: { allow: [root] },
  },
});
