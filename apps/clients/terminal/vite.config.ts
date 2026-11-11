import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { spawn } from "node:child_process";
import { mkdtempSync, readdirSync, rmSync, mkdirSync, copyFileSync } from "node:fs";
import { createInterface } from "node:readline";
import { createRequire } from "node:module";
const { labChannel } = createRequire(import.meta.url)("./electron/lab-channel.cjs") as {
  labChannel: (resolve: () => Promise<string>) => (payload: Uint8Array) => Promise<Buffer>;
};
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
            ASTERION_PYTHON: resolve(root, "build/electron-resources/python/bin/python3"),
            ASTERION_BUILTIN_RESEARCH:
              process.env.ASTERION_BUILTIN_RESEARCH ?? resolve(root, "research"),
          },
          stdio: ["pipe", "pipe", "pipe"],
          detached: true,
        });
        const core = {
          child,
          // Replies come in the order requests complete, each with its request's id.
          pending: new Map<
            number,
            { resolve: (response: string) => void; reject: (error: Error) => void }
          >(),
          next: 0,
          failed: null as Error | null,
        };
        const fail = (error: Error) => {
          core.failed = error;
          for (const item of core.pending.values()) item.reject(error);
          core.pending.clear();
        };
        createInterface({ input: child.stdout }).on("line", line => {
          const { id, reply } = JSON.parse(line) as { id: number; reply: unknown };
          core.pending.get(id)?.resolve(JSON.stringify(reply));
          core.pending.delete(id);
        });
        child.on("error", fail);
        child.on("exit", () => fail(new Error("C++ 核心已退出，请重新启动开发服务")));
        child.stderr.on("data", data => server.config.logger.error(String(data)));
        return core;
      };
      let core = startCore();
      const originalNode = process.env.ASTERION_ROOT;
      const send = (request: string) =>
        new Promise<string>((resolve, reject) => {
          const current = core;
          if (current.failed) return reject(current.failed);
          const id = ++current.next;
          current.pending.set(id, { resolve, reject });
          current.child.stdin.write(`{"id":${id},"request":${request}}\n`, error => {
            if (error) {
              current.failed = error;
              for (const item of current.pending.values()) item.reject(error);
              current.pending.clear();
            }
          });
        });
      const exited = (child: ReturnType<typeof spawn>) =>
        child.exitCode !== null || child.signalCode !== null
          ? Promise.resolve()
          : new Promise<void>(done => child.once("exit", () => done()));
      // Closing its input ends the core, which stops the services it started.
      const stopCore = async () => {
        const child = core.child;
        child.stdin.end();
        const timer = setTimeout(() => child.kill("SIGKILL"), 15000);
        await exited(child);
        clearTimeout(timer);
      };
      closeCore = async () => {
        process.off("SIGINT", interrupt);
        const failed = core.failed;
        await stopCore();
        if (failed) throw failed;
      };
      // Isolated end-to-end runs only: a fresh core and node directory per
      // spec file, so one spec's services and data never affect the next.
      const isolated = process.env.ASTERION_TEST_NODE_ISOLATED === "1" && !!originalNode;
      const reset = async () => {
        await stopCore();
        // The directory before goes, or a run of forty spec files fills the
        // temporary filesystem.
        for (const entry of readdirSync(originalNode!))
          rmSync(resolve(originalNode!, entry), {
            recursive: true,
            force: true,
            maxRetries: 20,
            retryDelay: 100,
          });
        // Inside the wrapper's temporary root, which it cleans up at exit.
        process.env.ASTERION_ROOT = mkdtempSync(resolve(originalNode!, "reset-"));
        const tls = process.env.ASTERION_TEST_WAREHOUSE_TLS;
        if (tls) {
          const target = resolve(process.env.ASTERION_ROOT!, "terminal/data/tls");
          mkdirSync(target, { recursive: true });
          for (const extension of ["crt", "key"])
            copyFileSync(
              resolve(tls, `reader.${extension}`),
              resolve(target, `client.${extension}`),
            );
        }
        core = startCore();
        // The new native host owns a different private Lab socket.
        lab = createLab();
        return process.env.ASTERION_ROOT;
      };
      const allowed = (
        req: { method?: string; headers: Record<string, unknown> },
        type = "application/json",
      ) => {
        // This middleware runs before Vite's own host check, so it enforces one
        // itself: a DNS-rebound page carries an attacker-chosen Host header.
        // Either content type makes a cross-site page ask a preflight first.
        const host = String(req.headers.host ?? "");
        return (
          req.method === "POST" &&
          req.headers["content-type"] === type &&
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
              res.end(JSON.stringify({ root: directory }));
            },
            error => {
              res.statusCode = 500;
              res.end(String(error));
            },
          );
        });
      // The page's lab requests, as the Electron main process forwards them.
      const createLab = () =>
        labChannel(async () => {
          const reply = JSON.parse(
            await send(JSON.stringify({ version: 1, method: "lab.endpoint", params: {} })),
          );
          if (reply.error) throw new Error(reply.error.message);
          return reply.result.lab_endpoint.endpoint as string;
        });
      let lab = createLab();
      server.middlewares.use("/__asterion/lab", (req, res) => {
        if (!allowed(req, "application/octet-stream")) {
          res.statusCode = 403;
          res.end("Forbidden");
          return;
        }
        const chunks: Buffer[] = [];
        let size = 0;
        req.on("data", (chunk: Buffer) => {
          size += chunk.length;
          if (size > 1024 * 1024) req.destroy();
          else chunks.push(chunk);
        });
        req.on("end", () => {
          lab(Buffer.concat(chunks))
            .then(reply => {
              res.setHeader("Content-Type", "application/octet-stream");
              res.end(reply);
            })
            .catch(error => {
              res.statusCode = 503;
              res.end(String(error));
            });
        });
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
  base: "./",
  plugins: [react(), ...(process.env.ASTERION_DESKTOP_DEV === "1" ? [] : [localCore()])],
  resolve: {
    alias: {
      "@asterion/workbench": resolve(root, "apps/clients/terminal/src/host"),
      "@asterion/desktop-bridge": resolve(root, "apps/clients/terminal/src/bridge"),
      "@asterion/terminal": resolve(root, "apps/clients/terminal/src"),
    },
  },
  // The desktop app loads one bundle from disk behind its startup screen, so
  // every workspace is ready on first click.
  build: { chunkSizeWarningLimit: 2048, sourcemap: true },
  server: {
    host: "127.0.0.1",
    port,
    strictPort: true,
    fs: { allow: [root] },
  },
});
