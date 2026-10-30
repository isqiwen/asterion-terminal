import { spawn } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve, join } from "node:path";
import { createInterface } from "node:readline";
import { once } from "node:events";
import { test as base } from "./test";
import { rpc } from "./dataset-fixture";
export { expect } from "./test";

// A test that uses tradingHost is tagged @trading: the trading service runs on
// Linux only, and CI runs these tests in its Linux job.
export const test = base.extend<{ tradingHost: { connection: string; restart(): Promise<void> } }>({
  tradingHost: async ({ request, isolatedNode }, runTest) => {
    void isolatedNode;
    test.skip(process.platform !== "linux", "Trading services run on Linux only");
    const snapshot = await rpc(request, "trading.identity.prepare");
    const root = await mkdtemp(join(tmpdir(), "asterion-trading-ui-"));
    const build = process.env.ASTERION_CPP_BUILD ?? resolve(__dirname, "../../../../build/Debug");
    const child = spawn(
      "python3",
      [
        resolve(__dirname, "../../../../tests/support/trading_peer.py"),
        root,
        build,
        snapshot.trading_fingerprint,
      ],
      { stdio: ["pipe", "pipe", "inherit"] },
    );
    const exited = once(child, "exit");
    const lines = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
    const next = async () => {
      const line = await lines.next();
      if (line.done) throw new Error("independent trading fixture exited");
      return JSON.parse(line.value);
    };
    try {
      const connection = "live-account";
      await rpc(request, "trading.connections.save", {
        id: connection,
        name: "live-account",
        revision: "",
        ...(await next()),
      });
      await runTest({
        connection,
        restart: async () => {
          child.stdin.write("restart\n");
          await next();
        },
      });
    } finally {
      child.stdin.end();
      await exited;
      await rm(root, { recursive: true, force: true });
    }
  },
});
