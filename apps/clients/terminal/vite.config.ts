import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
const root = fileURLToPath(new URL("../../..", import.meta.url));
// C++ build that serves the dev bridge; Windows desktop builds use Release.
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
      const executable =
        "asterion_terminal_dev_bridge" + (process.platform === "win32" ? ".exe" : "");
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
      const pending: { resolve: (response: string) => void; reject: (error: Error) => void }[] = [];
      let failed: Error | null = null;
      const fail = (error: Error) => {
        failed = error;
        for (const item of pending.splice(0)) item.reject(error);
      };
      createInterface({ input: child.stdout }).on("line", line => pending.shift()?.resolve(line));
      child.on("error", fail);
      child.on("exit", () => fail(new Error("C++ 核心已退出，请重新启动开发服务")));
      child.stderr.on("data", data => server.config.logger.error(String(data)));
      closeCore = async () => {
        process.off("SIGINT", interrupt);
        if (failed) {
          child.kill();
          throw failed;
        }
        if (process.env.ASTERION_NODE_DIRECTORY) {
          child.kill();
          return;
        }
        const result = await new Promise<string>((resolve, reject) => {
          pending.push({ resolve, reject });
          child.stdin.write(
            JSON.stringify({
              version: 1,
              method: "development.shutdown",
              params: { recover: false },
            }) + "\n",
          );
        });
        const response = JSON.parse(result);
        if (response.error) {
          server.config.logger.error(
            `Development cleanup incomplete; services may remain: ${response.error.message}`,
          );
          child.kill();
          throw new Error(response.error.message);
        }
        child.kill();
      };
      server.middlewares.use("/__asterion/api", (req, res) => {
        // This middleware runs before Vite's own host check, so it enforces one
        // itself: a DNS-rebound page carries an attacker-chosen Host header.
        const host = req.headers.host ?? "";
        if (
          req.method !== "POST" ||
          req.headers["content-type"] !== "application/json" ||
          !allowedHosts.has(host) ||
          (req.headers.origin && req.headers.origin !== `http://${host}`)
        ) {
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
          if (failed) {
            res.statusCode = 503;
            res.end(failed.message);
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
          new Promise<string>((resolve, reject) => {
            pending.push({ resolve, reject });
            child.stdin.write(request + "\n", error => {
              if (error) fail(error);
            });
          })
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
