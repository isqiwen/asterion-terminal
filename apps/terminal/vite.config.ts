import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
const root = fileURLToPath(new URL("../..", import.meta.url));
const port = 1420;
const allowedHosts = new Set([`127.0.0.1:${port}`, `localhost:${port}`]);

// Development-only transport to the same C++ API used by Tauri. No fake backend.
function localCore(): Plugin {
  return {
    name: "asterion-local-core",
    configureServer(server) {
      const executable =
        "asterion_terminal_dev_bridge" + (process.platform === "win32" ? ".exe" : "");
      const child = spawn(resolve(root, "build/Debug", executable), [], {
        env: {
          ...process.env,
          ASTERION_REMOTE_RESOURCES:
            process.env.ASTERION_REMOTE_RESOURCES ??
            resolve(root, "apps/terminal/src-tauri/resources/remote-linux"),
        },
        stdio: ["pipe", "pipe", "pipe"],
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
      server.httpServer?.once("close", () => child.kill());
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
  root: resolve(root, "apps/terminal"),
  plugins: [react(), localCore()],
  resolve: {
    alias: {
      "@asterion/workbench": resolve(root, "apps/terminal/src/host"),
      "@asterion/desktop-bridge": resolve(root, "apps/terminal/src/bridge"),
      "@asterion/overview": resolve(root, "apps/terminal/plugins/overview"),
      "@asterion/terminal": resolve(root, "apps/terminal/src"),
    },
  },
  server: { host: "127.0.0.1", port, strictPort: true, fs: { allow: [root] } },
});
